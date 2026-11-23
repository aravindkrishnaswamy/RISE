//////////////////////////////////////////////////////////////////////
//
//  FireParameterSchema.cpp - see FireParameterSchema.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireParameterSchema.h"
#include "FireSolver.h"
#include "../../Utilities/FireSequence.h"

#include <algorithm>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool Number(const V* v, double& x)
{
	if (!v) return false;
	if (v->GetType() == V::Float64) x = v->GetFloat();
	else if (v->GetType() == V::UnsignedInteger) x = static_cast<double>(v->GetIntegerArgument());
	else return false;
	return true;
}

// Every default the schema states, made explicit.
bool Complete(const ParameterSchema& schema, const V& authored, V& completed, std::string& error)
{
	if (authored.GetType() != V::Map) return Fail(error, "fire parameters must be a map");
	V::Members m = authored.GetMap();
	for (const auto& p : schema.parameters) {
		if (authored.Find(p.name)) continue;
		V value;
		if (SchemaDefaultValue(p, value)) m.emplace_back(p.name, value);
		else if (p.type == SchemaType::Text) m.emplace_back(p.name, V::String(""));   // unset: the library default
	}
	completed = V::MapValue(m);
	RISECBOR64::Bytes probe;
	return RISECBOR64::Encode(completed, probe, &error);
}

// What FireSolver::Configure returns for completed parameters: the same map,
// with the frame window the solver derives (frame_origin_s, end_time_s) taken
// from the cache's last bake when the scene does not state it.
bool Predict(const V& completed, const V* baked, V& effective, std::uint64_t& frames, std::string& error)
{
	V::Members m = completed.GetMap();
	for (const char* derived : {"frame_origin_s", "end_time_s"}) {
		if (completed.Find(derived)) continue;
		const V* b = baked ? baked->Find(derived) : nullptr;
		if (!b) return Fail(error, std::string(derived) + " is derived by the fire solver; state it, or bake once, "
			"for the scene to key this simulation");
		m.emplace_back(derived, *b);
	}
	effective = V::MapValue(m);
	double origin = 0, end = 0, cadence = 0;
	if (!Number(effective.Find("frame_origin_s"), origin) || !Number(effective.Find("end_time_s"), end) ||
		!Number(effective.Find("frame_cadence_s"), cadence) || !(cadence > 0))
		return Fail(error, "fire frame window is invalid");
	frames = 0;
	while (frames < (std::uint64_t(1) << 24) && origin + static_cast<double>(frames)*cadence <= end) ++frames;
	RISECBOR64::Bytes probe;
	return RISECBOR64::Encode(effective, probe, &error);
}

ParameterSchema Build()
{
	ParameterSchema s;
	s.solverId = Implementation::FireMethaneSolverId;
	s.solverVersion = Implementation::FireMethaneSolverVersion;
	s.summary = "A methane pool fire simulated by the fire.methane solver (the production capstone case: a "
		"0.30 m pool at 33 kW; Metal; baked by `rise-sim bake <scene>`, never live).  Publishes `volume` "
		"(grid.v1: carbon, temperature and the chemistry channels a fire_medium binds).  Parameters and their "
		"legacy environment names: docs/SIMULATION_M2_RESULTS.md.  A scene bake passes every option "
		"explicitly, so the legacy RISE_FIRE_* environment never changes a scene's fire.";
	auto P = [&s](const char* name, SchemaType type, const char* def, const char* units, const std::string& doc)
		-> SchemaParameter& {
		s.parameters.emplace_back();
		SchemaParameter& p = s.parameters.back();
		p.name = name; p.type = type; p.defaultText = def; p.units = units; p.doc = doc;
		return p;
	};
	auto range = [](SchemaParameter& p, double lo, double hi) { p.hasRange = true; p.minimum = lo; p.maximum = hi; };
	{ auto& p = P("tier", SchemaType::Count, "8", "", "Resolution tier: 6, 8 or 10 (the capstone tiers)"); range(p, 6, 10); }
	{ auto& p = P("frame_cadence_s", SchemaType::Real, "0.0625", "s", "Output frame cadence (only 0.0625)"); range(p, 0.0625, 0.0625); }
	P("frame_origin_s", SchemaType::Real, "", "s", "Frame clock origin, in fire time (default: the statistics start, "
		"5 flow-through times, derived by the solver)");
	P("end_time_s", SchemaType::Real, "", "s", "Absolute end of the window, in fire time (default: the window target, "
		"derived by the solver); shorter = a bounded window");
	{ auto& p = P("pool_diameter_m", SchemaType::Real, "0.3", "m", "Pool diameter (only the certified 0.30 m)"); range(p, 0.3, 0.3); }
	{ auto& p = P("heat_release_rate_kw", SchemaType::Real, "33", "kW", "Heat release rate (only the certified 33 kW)"); range(p, 33, 33); }
	{ auto& p = P("mode", SchemaType::Enum, "certified", "", "certified (bit-reproducible) or fast (r275 uncertified fast mode)");
	  p.values = {"certified", "fast"}; }
	for (const FireSolverOptionInfo& o : FireSolverOptionInfos()) {
		const std::string doc = "Result-affecting fire option (legacy " + o.environment + ")";
		if (o.kind == "flag") {
			P(o.parameter.c_str(), SchemaType::Bool, "FALSE", "", doc);
		} else if (o.kind == "level") {
			auto& p = P(o.parameter.c_str(), SchemaType::Count, "0", "", doc + "; 0 = unset");
			range(p, 0, o.maxLevel);
		} else {
			auto& p = P(o.parameter.c_str(), SchemaType::Text, o.defaultText.c_str(), "",
				doc + (o.defaultText.empty() ? "; unset = the library default" : ""));
			p.validText = o.valid;
		}
	}
	s.outputs = {{"volume", "grid.v1", "The fire's sparse grid: carbon, temperature, the chemistry channels, velocity"}};
	// Called after the schema exists (the scene keys and bakes use it).
	s.completeParameters = [](const V& authored, V& completed, std::string& error) {
		return Complete(FireParameterSchema(), authored, completed, error);
	};
	s.predictEffectiveParameters = Predict;
	return s;
}
} // namespace

const ParameterSchema& FireParameterSchema()
{
	static const ParameterSchema schema = Build();
	return schema;
}

} }
