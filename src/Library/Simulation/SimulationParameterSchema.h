// Simulation M4: a solver's parameter schema -- NOT part of the solver
// contract (docs/SIMULATION_SOLVER_CONTRACT.md).  The contract's Configure
// returns effective parameters but says nothing about the names, types,
// defaults, ranges, units or documentation of the parameters a solver
// accepts.  A scene producer chunk (`cloth_simulation`, `fire_simulation`)
// needs exactly that to describe itself to the parser, the editor's
// properties panel, the suggestion engine and the agent schema, so each
// built-in solver states it once here, beside its factory, and the chunk
// descriptor is generated from it.  An additive registry feature: a solver
// without a schema still works through the contract; it just has no chunk.
//
// The schema is the declared parameter set.  Configure stays authoritative
// for validation and canonicalisation; tests check that every schema default
// is what Configure applies (SimulationSceneTest).
#ifndef RISE_SIMULATION_PARAMETER_SCHEMA_H
#define RISE_SIMULATION_PARAMETER_SCHEMA_H

#include "../Utilities/RISECBOR64.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

struct SolverDescription;   // SimulationSolver.h (M5c: ParameterSchema::describeUnkeyed)

enum class SchemaType {
	Bool,     // TRUE / FALSE in a scene; CBOR boolean
	Count,    // non-negative integer; CBOR unsigned
	Real,     // finite number; CBOR float
	Text,     // free text; CBOR text
	Enum,     // one of `values`; CBOR text
	Vector3   // three finite numbers; CBOR array of three floats
};

//! One top-level solver parameter.  A chunk spells it with the same name.
struct SchemaParameter {
	std::string name;
	SchemaType type = SchemaType::Real;
	std::string defaultText;            // the default in scene syntax ("" = the solver derives it)
	std::string modeDefaults;           // defaults that depend on another parameter, e.g. "interactive: 2"
	bool hasRange = false;
	double minimum = 0, maximum = 0;    // inclusive, when hasRange
	std::vector<std::string> values;    // Enum
	std::string units, doc;
	bool keyed = true;                  // false: never enters the cache key (pace, threads)
	bool (*validText)(const std::string&) = nullptr; // Text: the solver's own validator, when it has one
};

//! A chunk-only field that is not a solver parameter of its own: a
//! structured form (cloth pins and colliders) or a reference to a scene
//! element the simulation reads.  The schema's `translate` turns these into
//! solver parameters.
struct SchemaChunkField {
	enum class Kind { Text, ObjectReference };
	std::string name;
	Kind kind = Kind::Text;
	bool repeatable = false;
	std::string defaultText;
	std::vector<std::string> values;    // when nonempty, the field is an enum
	std::string doc;
};

//! A cache output a configuration of the solver publishes.
struct SchemaOutput {
	std::string name, codec, doc;
};

//! Authored chunk values by field name (a single-valued field has one entry).
using SchemaFieldValues = std::map<std::string, std::vector<std::string>>;

struct SchemaTranslation {
	RISECBOR64::Value::Members parameters; // appended to the flat parameters
	std::vector<std::string> sceneInputs;  // scene elements (objects) read, in first-use order
};

struct ParameterSchema {
	std::string solverId;
	std::string summary;                // one paragraph for the chunk description
	std::vector<SchemaParameter> parameters;
	std::vector<SchemaChunkField> chunkFields;
	std::vector<SchemaOutput> outputs;
	//! Turns the chunk-only fields (all of `chunkFields`) into solver
	//! parameters; null when there are none.
	std::function<bool(const SchemaFieldValues&, SchemaTranslation&, std::string&)> translate;

	//! For a solver that must not be configured inside an application
	//! process (fire: one configuration per process, its Configure writes
	//! the process environment and dispatches GPU checks), the scene keys it
	//! without Configure:
	//!   completeParameters: the authored map with every default the schema
	//!     states made explicit -- what a scene bake passes to Configure, so a
	//!     legacy environment default never reaches a scene's simulation;
	//!   predictEffectiveParameters: the effective parameters Configure would
	//!     return for those, given the effective parameters of the cache's
	//!     last bake (`baked`, may be null) for the values the solver derives;
	//!     false when they cannot be known without configuring (no bake yet);
	//!     also the output count.
	//! The cache key is then MakeCacheKey(solverId, solverVersion, predicted,
	//! {}) -- equal to the worker's key exactly when the prediction is right
	//! (a wrong prediction shows the cache as stale, never as current).
	std::string solverVersion;
	std::function<bool(const RISECBOR64::Value& authored, RISECBOR64::Value& completed, std::string&)> completeParameters;
	std::function<bool(const RISECBOR64::Value& completed, const RISECBOR64::Value* baked, RISECBOR64::Value& effective,
		std::uint64_t& frames, std::string&)> predictEffectiveParameters;
	//! M5c, additive: for a predicted (unconfigured) solver, the description
	//! fields that are NOT keyed and that the prediction can still know from
	//! the effective parameters -- the time support (cache startTime/interval)
	//! and the Capabilities cost/memory estimates.  Null: left at their
	//! defaults.  Never changes the key, the epoch or the outputs.
	std::function<void(const RISECBOR64::Value& effective, SolverDescription& d)> describeUnkeyed;
};

//! A schema entry by name, or null.
const SchemaParameter* FindSchemaParameter(const ParameterSchema& schema, const std::string& name);

//! One scene-syntax value of a parameter -> its canonical CBOR value.
//! Checks the type, the range and the enum values.
bool SchemaValueFromText(const SchemaParameter& parameter, const std::string& text, RISECBOR64::Value& value,
	std::string& error);
//! A default (defaultText) as CBOR; false when the parameter has none.
bool SchemaDefaultValue(const SchemaParameter& parameter, RISECBOR64::Value& value);

//! The chunk's authored values -> the solver parameter map: every flat
//! parameter the chunk sets, then the schema's translation of its chunk-only
//! fields.  Parameters the chunk does not set are left to the solver's
//! defaults.  Unknown field names are refused (the chunk descriptor already
//! refuses them at parse time).
bool TranslateSchemaChunk(const ParameterSchema& schema, const SchemaFieldValues& values,
	RISECBOR64::Value& parameters, std::vector<std::string>& sceneInputs, std::string& error);

// Process-wide registry, beside the solver registry (RegisterSolver).
bool RegisterSolverSchema(const std::string& solverId, std::function<const ParameterSchema&()> schema,
	std::string& error);
//! Null when the solver registered no schema.
const ParameterSchema* FindSolverSchema(const std::string& solverId);
std::vector<std::string> SolversWithSchemas();

} }
#endif
