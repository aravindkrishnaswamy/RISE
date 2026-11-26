#include "pch.h"
#include "SimulationParameterSchema.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <mutex>
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

bool ParseReal(const std::string& token, double& x)
{
	if (token.empty()) return false;
	const char* begin = token.c_str();
	char* end = nullptr;
	errno = 0;
	x = std::strtod(begin, &end);
	return end && *end == '\0' && errno == 0 && std::isfinite(x);
}

struct Registry {
	std::mutex mutex;
	std::vector<std::pair<std::string, std::function<const ParameterSchema&()>>> schemas;
};
Registry& TheRegistry()
{
	static Registry registry;
	return registry;
}
} // namespace

const SchemaParameter* FindSchemaParameter(const ParameterSchema& schema, const std::string& name)
{
	for (const auto& p : schema.parameters) if (p.name == name) return &p;
	return nullptr;
}

bool SchemaValueFromText(const SchemaParameter& p, const std::string& text, V& value, std::string& error)
{
	const auto tokens = Tokens(text);
	const std::string where = "parameter " + p.name;
	auto range = [&](double x) {
		return !p.hasRange || (x >= p.minimum && x <= p.maximum);
	};
	auto rangeText = [&]() {
		std::ostringstream s;
		s << " (allowed " << p.minimum << " to " << p.maximum << ")";
		return s.str();
	};
	switch (p.type) {
	case SchemaType::Bool: {
		if (tokens.size() != 1) return Fail(error, where + " must be TRUE or FALSE");
		std::string t = tokens[0];
		std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		if (t == "TRUE" || t == "1" || t == "YES") value = V::Bool(true);
		else if (t == "FALSE" || t == "0" || t == "NO") value = V::Bool(false);
		else return Fail(error, where + " must be TRUE or FALSE");
		return true;
	}
	case SchemaType::Count: {
		double x = 0;
		if (tokens.size() != 1 || !ParseReal(tokens[0], x) || x < 0 || std::floor(x) != x || x > 9.0e15)
			return Fail(error, where + " must be a non-negative integer");
		if (!range(x)) return Fail(error, where + " is out of range" + rangeText());
		value = V::Unsigned(static_cast<std::uint64_t>(x));
		return true;
	}
	case SchemaType::Real: {
		double x = 0;
		if (tokens.size() != 1 || !ParseReal(tokens[0], x)) return Fail(error, where + " must be a finite number");
		if (!range(x)) return Fail(error, where + " is out of range" + rangeText());
		value = V::Float(x);
		return true;
	}
	case SchemaType::Text:
		if (text.empty()) return Fail(error, where + " must not be empty");
		if (p.validText && !p.validText(text)) return Fail(error, where + " has an invalid value '" + text + "'");
		value = V::String(text);
		return true;
	case SchemaType::Enum:
		if (tokens.size() != 1 || std::find(p.values.begin(), p.values.end(), tokens[0]) == p.values.end()) {
			std::string allowed;
			for (const auto& v : p.values) allowed += (allowed.empty() ? "" : "|") + v;
			return Fail(error, where + " must be one of " + allowed);
		}
		value = V::String(tokens[0]);
		return true;
	case SchemaType::Vector3: {
		V::Values xyz;
		for (const auto& t : tokens) {
			double x = 0;
			if (!ParseReal(t, x)) return Fail(error, where + " must be three finite numbers");
			xyz.push_back(V::Float(x));
		}
		if (xyz.size() != 3) return Fail(error, where + " must be three finite numbers");
		value = V::ArrayValue(xyz);
		return true;
	}
	}
	return Fail(error, where + " has an unknown type");
}

bool SchemaDefaultValue(const SchemaParameter& p, V& value)
{
	if (p.defaultText.empty()) return false;
	std::string ignored;
	return SchemaValueFromText(p, p.defaultText, value, ignored);
}

bool TranslateSchemaChunk(const ParameterSchema& schema, const SchemaFieldValues& values, V& parameters,
	std::vector<std::string>& sceneInputs, std::string& error)
{
	V::Members members;
	SchemaFieldValues structured;
	for (const auto& field : values) {
		if (const SchemaParameter* p = FindSchemaParameter(schema, field.first)) {
			if (field.second.size() != 1) return Fail(error, "parameter " + field.first + " is given more than once");
			V value;
			if (!SchemaValueFromText(*p, field.second.front(), value, error)) return false;
			members.emplace_back(p->name, value);
			continue;
		}
		const bool known = std::any_of(schema.chunkFields.begin(), schema.chunkFields.end(),
			[&](const SchemaChunkField& f) { return f.name == field.first; });
		if (!known) return Fail(error, "unknown simulation parameter " + field.first);
		structured.insert(field);
	}
	SchemaTranslation translation;
	if (schema.translate && !schema.translate(structured, translation, error)) return false;
	for (auto& m : translation.parameters) {
		for (const auto& existing : members)
			if (existing.first == m.first) return Fail(error, "parameter " + m.first + " is given twice");
		members.push_back(std::move(m));
	}
	V candidate = V::MapValue(members);
	RISECBOR64::Bytes probe;
	if (!RISECBOR64::Encode(candidate, probe, &error)) return false;   // duplicate or non-NFC keys
	parameters = std::move(candidate);
	sceneInputs = std::move(translation.sceneInputs);
	return true;
}

bool RegisterSolverSchema(const std::string& solverId, std::function<const ParameterSchema&()> schema,
	std::string& error)
{
	if (solverId.empty() || !schema) return Fail(error, "invalid solver schema registration");
	auto& r = TheRegistry();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& s : r.schemas) if (s.first == solverId) return true;   // idempotent
	r.schemas.emplace_back(solverId, std::move(schema));
	return true;
}

const ParameterSchema* FindSolverSchema(const std::string& solverId)
{
	std::function<const ParameterSchema&()> found;
	{
		auto& r = TheRegistry();
		std::lock_guard<std::mutex> hold(r.mutex);
		for (const auto& s : r.schemas) if (s.first == solverId) found = s.second;
	}
	return found ? &found() : nullptr;
}

std::vector<std::string> SolversWithSchemas()
{
	auto& r = TheRegistry();
	std::lock_guard<std::mutex> hold(r.mutex);
	std::vector<std::string> ids;
	for (const auto& s : r.schemas) ids.push_back(s.first);
	std::sort(ids.begin(), ids.end());
	return ids;
}

} }
