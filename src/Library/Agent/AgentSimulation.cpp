#include "pch.h"
#include "AgentSimulation.h"

#include "../Interfaces/IJobPriv.h"
#include "../SceneEditor/SceneEditController.h"
#include "../SceneEditor/FileIdentity.h"
#include "../SceneEditor/LiveSimulation.h"
#include "../Simulation/SimulationBakeManager.h"
#include "../Simulation/BuiltinSolvers.h"
#include "../Simulation/SimulationParameterSchema.h"
#include "../Simulation/SimulationProcess.h"
#include "../Simulation/SimulationCache.h"
#include "../Cst/Cst.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			const int kInvalidParams = -32602;
			using Simulation::BakeJob;
			using Simulation::SimulationBakeManager;

			SimulationBakeManager& Bakes() { return SimulationBakeManager::Instance(); }

			JsonValue Str( const std::string& s ) { return JsonValue::MakeString( s ); }
			JsonValue Num( double d ) { return JsonValue::MakeNumber( d ); }
			JsonValue Bool( bool b ) { return JsonValue::MakeBool( b ); }

			JsonValue StringArray( const std::vector<std::string>& v )
			{
				JsonValue a = JsonValue::MakeArray();
				for( const auto& s : v ) a.push_back( Str( s ) );
				return a;
			}

			JsonValue JobJson( const BakeJob& j )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "jobId", Num( static_cast<double>( j.id ) ) );
				o.set( "simulation", Str( j.simulation ) );
				o.set( "state", Str( Simulation::BakeJobStateName( j.state ) ) );
				o.set( "terminal", Bool( Simulation::BakeJobTerminal( j.state ) ) );
				o.set( "resumable", Bool( j.resumable ) );
				o.set( "adopted", Bool( j.adopted ) );
				o.set( "interrupted", Bool( j.interrupted ) );
				o.set( "resume", Bool( j.resume ) );
				o.set( "force", Bool( j.force ) );
				o.set( "solver", Str( j.solverId ) );
				o.set( "backend", Str( j.backend ) );
				o.set( "cacheDir", Str( j.cacheDir.string() ) );
				o.set( "simulatedTime", Num( j.simulatedTime ) );
				o.set( "endTime", Num( j.endTime ) );
				o.set( "framesWritten", Num( static_cast<double>( j.framesWritten ) ) );
				o.set( "framesTotal", Num( static_cast<double>( j.framesTotal ) ) );
				o.set( "progress", Num( j.framesTotal ? static_cast<double>( j.framesWritten ) / static_cast<double>( j.framesTotal ) : 0.0 ) );
				o.set( "wallSeconds", Num( j.wallSeconds ) );
				o.set( "etaSeconds", j.etaSeconds >= 0 ? Num( j.etaSeconds ) : JsonValue::MakeNull() );
				o.set( "pid", Num( static_cast<double>( j.pid ) ) );
				JsonValue deps = JsonValue::MakeArray();
				for( const auto d : j.dependsOn ) deps.push_back( Num( static_cast<double>( d ) ) );
				o.set( "dependsOn", deps );
				if( !j.waitingFor.empty() ) o.set( "waitingFor", Str( j.waitingFor ) );
				if( !j.detail.empty() ) o.set( "detail", Str( j.detail ) );
				if( !j.lastError.empty() ) o.set( "error", Str( j.lastError ) );
				if( !j.logFile.empty() ) o.set( "log", Str( j.logFile.string() ) );
				return o;
			}

			const char* KeyStatus( const Simulation::SceneSimulationStatus& s )
			{
				if( s.expectedKey.empty() ) return "unknown";
				if( s.cacheKey.empty() ) return "missing";
				return s.cacheKey == s.expectedKey ? "current" : "stale";
			}

			std::string Trimmed( const std::string& t )
			{
				const auto b = t.find_first_not_of( " \t\r\n" );
				if( b == std::string::npos ) return std::string();
				return t.substr( b, t.find_last_not_of( " \t\r\n" ) - b + 1 );
			}

			const char* SchemaTypeName( Simulation::SchemaType t )
			{
				switch( t ) {
					case Simulation::SchemaType::Bool: return "bool";
					case Simulation::SchemaType::Count: return "count";
					case Simulation::SchemaType::Real: return "real";
					case Simulation::SchemaType::Text: return "text";
					case Simulation::SchemaType::Enum: return "enum";
					case Simulation::SchemaType::Vector3: return "vector3";
				}
				return "text";
			}

			//! One-line statement of what a parameter accepts, for refusals.
			std::string DescribeAccepted( const Simulation::SchemaParameter& p )
			{
				std::string out = std::string( SchemaTypeName( p.type ) );
				if( p.type == Simulation::SchemaType::Enum ) {
					out += " (";
					for( std::size_t i = 0; i < p.values.size(); ++i ) out += ( i ? ", " : "" ) + p.values[i];
					out += ")";
				}
				if( p.hasRange ) {
					char buf[96];
					std::snprintf( buf, sizeof( buf ), " in [%g, %g]", p.minimum, p.maximum );
					out += buf;
				}
				if( !p.units.empty() ) out += " " + p.units;
				return out;
			}

			//! The simulation's status row, or a refusal message naming the ones that exist.
			const Simulation::SceneSimulationStatus* FindSimulation( const Simulation::BakeSceneSnapshot& snapshot,
				const std::string& name, std::string& error )
			{
				for( const auto& s : snapshot.simulations ) if( s.name == name ) return &s;
				error = "no simulation named `" + name + "`";
				if( snapshot.simulations.empty() ) error += " (this scene declares none)";
				else {
					error += "; simulations: ";
					for( std::size_t i = 0; i < snapshot.simulations.size(); ++i )
						error += ( i ? ", " : "" ) + snapshot.simulations[i].name;
				}
				return nullptr;
			}

			bool Refuse( JsonValue& result, const std::string& message )
			{
				result = JsonValue::MakeObject();
				result.set( "ok", Bool( false ) );
				result.set( "message", Str( message ) );
				return true;
			}

			bool Unsupported( JsonValue& result, const std::string& verb )
			{
				result = JsonValue::MakeObject();
				result.set( "ok", Bool( false ) );
				result.set( "supported", Bool( false ) );
				result.set( "message", Str( verb + " needs a live in-app viewport; this session is headless "
					"(rise --agent-stdio / --mcp). Bake with sim_bake instead: it runs rise-sim and works headless." ) );
				return true;
			}

			// 1 = present and valid, 0 = absent, -1 = invalid (errorMessage set).
			int ParseJobId( const JsonValue& params, std::uint64_t& id, std::string& errorMessage )
			{
				const JsonValue* v = params.find( "jobId" );
				if( !v || v->isNull() ) return 0;
				const double d = v->asNumber( -1.0 );
				if( !v->isNumber() || !std::isfinite( d ) || !( d >= 0.0 && d <= 9007199254740992.0 ) ) {
					errorMessage = "Invalid params: 'jobId' must be a finite, non-negative number";
					return -1;
				}
				id = static_cast<std::uint64_t>( d );
				return 1;
			}

			int ParseString( const JsonValue& params, const char* field, std::string& out, std::string& errorMessage )
			{
				const JsonValue* v = params.find( field );
				if( !v || v->isNull() ) return 0;
				if( !v->isString() ) {
					errorMessage = std::string( "Invalid params: '" ) + field + "' must be a string";
					return -1;
				}
				out = v->asString();
				return 1;
			}

			int ParseBool( const JsonValue& params, const char* field, bool& out, std::string& errorMessage )
			{
				const JsonValue* v = params.find( field );
				if( !v || v->isNull() ) return 0;
				if( !v->isBool() ) {
					errorMessage = std::string( "Invalid params: '" ) + field + "' must be a boolean";
					return -1;
				}
				out = v->asBool();
				return 1;
			}

			int ParseNumber( const JsonValue& params, const char* field, double& out, std::string& errorMessage )
			{
				const JsonValue* v = params.find( field );
				if( !v || v->isNull() ) return 0;
				if( !v->isNumber() || !std::isfinite( v->asNumber() ) ) {
					errorMessage = std::string( "Invalid params: '" ) + field + "' must be a finite number";
					return -1;
				}
				out = v->asNumber();
				return 1;
			}
		}

		bool IsAgentSimulationVerb( const std::string& m )
		{
			return m == "sim_list" || m == "sim_status" || m == "sim_wait" || m == "sim_cache" ||
				m == "sim_param_get" || IsAgentSimulationControlVerb( m );
		}

		bool IsAgentSimulationControlVerb( const std::string& m )
		{
			return m == "sim_bake" || m == "sim_cancel" || m == "sim_live_start" || m == "sim_live_stop" || m == "sim_seek" ||
				m == "sim_param_set";
		}

		bool AgentSimulationSnapshot( IJobPriv& job, SceneEditController* controller,
			Simulation::BakeSceneSnapshot& snapshot, std::string& error )
		{
			// A simulation's key needs its solver registered in THIS process
			// (the headless CLI never registers them; the worker does its own).
			// Idempotent; registering configures nothing.
			static const bool registered = [] {
				std::string ignored;
				return Simulation::RegisterBuiltinSolvers( ignored );
			}();
			(void)registered;
			if( controller ) return controller->GetSimulationBakeSnapshot( snapshot, /*blocking=*/true, error );
			snapshot = Simulation::BakeSceneSnapshot();
			snapshot.scene = job.GetCstLoadFileIdentity().filePath;
			if( !job.DescribeSimulations( snapshot.simulations, error ) ) return false;
			// Headless: no dirty trackers; the serialized Document against the file decides.
			if( !snapshot.scene.empty() ) {
				const RISE::Cst::Document* doc = job.HasRetainedCstDocument() ? job.GetCstDocument() : nullptr;
				std::ifstream in( snapshot.scene, std::ios::binary );
				if( !doc || !in ) snapshot.unsavedChanges = true;
				else {
					const std::string disk( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
					snapshot.unsavedChanges = disk != RISE::Cst::SerializeCst( *doc );
				}
			}
			return true;
		}

		bool ValidateSimulationParamEdit( IJobPriv& job, SceneEditController* controller,
			const std::string& simulation, const std::string& param, const std::string& value,
			bool hasOccurrence, int occurrence, std::string& error )
		{
			Simulation::BakeSceneSnapshot snapshot;
			if( !AgentSimulationSnapshot( job, controller, snapshot, error ) ) return false;
			const Simulation::SceneSimulationStatus* sim = FindSimulation( snapshot, simulation, error );
			if( !sim ) return false;
			const Simulation::ParameterSchema* schema = sim->solverId.empty() ? nullptr : Simulation::FindSolverSchema( sim->solverId );
			if( !schema ) {
				error = "`" + simulation + "` is a " + sim->keyword + ( sim->solverId.empty() ?
					" (no solver parameters to validate; edit it with propose_patch)" : " whose solver registered no parameter schema" );
				return false;
			}
			if( const Simulation::SchemaParameter* p = Simulation::FindSchemaParameter( *schema, param ) ) {
				if( hasOccurrence && occurrence != 0 ) {
					error = "`" + param + "` appears once on a simulation; `occurrence` applies to repeatable fields only";
					return false;
				}
				RISECBOR64::Value ignored;
				std::string why;
				if( !Simulation::SchemaValueFromText( *p, value, ignored, why ) ) {
					error = "`" + param + "` = `" + value + "` is refused by the " + sim->solverId + " schema: " + why +
						" (accepts " + DescribeAccepted( *p ) + ")";
					return false;
				}
				return true;
			}
			for( const auto& f : schema->chunkFields ) {
				if( f.name != param ) continue;
				if( !f.values.empty() && std::find( f.values.begin(), f.values.end(), Trimmed( value ) ) == f.values.end() ) {
					error = "`" + param + "` = `" + value + "` is not one of: ";
					for( std::size_t i = 0; i < f.values.size(); ++i ) error += ( i ? ", " : "" ) + f.values[i];
					return false;
				}
				if( Trimmed( value ).empty() ) { error = "`" + param + "` needs a value"; return false; }
				if( hasOccurrence && !f.repeatable && occurrence != 0 ) {
					error = "`" + param + "` is not repeatable; `occurrence` must be 0 or absent";
					return false;
				}
				if( hasOccurrence && f.repeatable && job.HasRetainedCstDocument() && job.GetCstDocument() ) {
					const RISE::Cst::Document* doc = job.GetCstDocument();
					const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, simulation, nullptr, "simulation", false );
					const RISE::Cst::NodeRef chunk = id != 0 ? RISE::Cst::DocResolveNodeId( *doc, id ) : RISE::Cst::NodeRef();
					const int n = chunk ? RISE::Cst::ParamOccurrenceCount( chunk, param ) : 0;
					if( occurrence >= n ) {
						error = "`" + simulation + "` has " + std::to_string( n ) + " `" + param + "` line(s); occurrence " +
							std::to_string( occurrence ) + " does not exist (add lines with propose_patch/insert_chunk)";
						return false;
					}
				}
				return true;   // text forms (pins, colliders) and object references: the edit's own dry-run derive checks them
			}
			error = "`" + sim->solverId + "` has no parameter `" + param + "`";
			error += "; it has: ";
			bool first = true;
			for( const auto& p : schema->parameters ) { error += ( first ? "" : ", " ) + p.name; first = false; }
			for( const auto& f : schema->chunkFields ) { error += ( first ? "" : ", " ) + f.name; first = false; }
			error += ". (`name`, `cache` and `port_source` are edited with propose_patch.)";
			return false;
		}

		bool HandleAgentSimulationVerb( const std::string& m, const JsonValue& params,
			IJobPriv& job, SceneEditController* controller,
			JsonValue& result, int& errorCode, std::string& errorMessage )
		{
			errorCode = kInvalidParams;
			result = JsonValue::MakeObject();

			//------------------------------------------------------------------
			// sim_list {} -> {ok, scene, unsavedChanges, liveSupported, simulations:[...]}
			//------------------------------------------------------------------
			if( m == "sim_list" ) {
				Simulation::BakeSceneSnapshot snapshot;
				std::string error;
				if( !AgentSimulationSnapshot( job, controller, snapshot, error ) ) return Refuse( result, error );
				Bakes().Attach( snapshot, error );
				const std::string key = SimulationBakeManager::SceneKey( snapshot.scene );
				JsonValue list = JsonValue::MakeArray();
				for( const auto& s : snapshot.simulations ) {
					JsonValue o = JsonValue::MakeObject();
					o.set( "name", Str( s.name ) );
					o.set( "keyword", Str( s.keyword ) );
					o.set( "solver", Str( s.solverId ) );
					o.set( "state", Str( Simulation::SceneSimulationStateName( s.state ) ) );
					o.set( "upstreamOutOfDate", Bool( s.upstreamOutOfDate ) );
					o.set( "keyStatus", Str( KeyStatus( s ) ) );
					o.set( "expectedKey", Str( s.expectedKey ) );
					o.set( "cacheKey", Str( s.cacheKey ) );
					o.set( "fidelity", Str( s.fidelity ) );
					o.set( "cacheDir", Str( s.cacheDir.string() ) );
					o.set( "interactive", Bool( s.interactive ) );
					o.set( "backend", Str( s.backend ) );
					o.set( "framesAvailable", Num( static_cast<double>( s.framesAvailable ) ) );
					o.set( "framesTotal", Num( static_cast<double>( s.framesTotal ) ) );
					o.set( "startTime", Num( s.startTime ) );
					o.set( "interval", Num( s.interval ) );
					o.set( "endTime", Num( s.framesTotal ? s.startTime + static_cast<double>( s.framesTotal - 1 ) * s.interval : s.startTime ) );
					o.set( "upstream", StringArray( s.upstream ) );
					o.set( "inputs", StringArray( s.inputs ) );
					// M5c: the estimates before a bake (0 = unknown; approximate) and why a cache is stale.
					o.set( "memoryEstimate", Num( static_cast<double>( s.memoryEstimate ) ) );
					o.set( "costEstimate", Num( s.costEstimate ) );
					o.set( "wallEstimateSeconds", Num( s.wallEstimate ) );
					o.set( "estimate", Str( Simulation::FormatSimulationEstimate( s ) ) );
					if( !s.staleReason.empty() ) o.set( "staleReason", Str( s.staleReason ) );
					if( !s.error.empty() ) o.set( "error", Str( s.error ) );
					// GUI status pass: what stands behind the label, and the couplings.
					o.set( "evidenceScope", Str( s.evidenceScope ) );
					o.set( "evidenceCases", Num( static_cast<double>( s.evidenceCases ) ) );
					o.set( "fidelityLimit", Str( s.fidelityLimit ) );
					o.set( "connections", StringArray( s.connections ) );
					o.set( "meshConsumers", StringArray( s.meshConsumers ) );
					BakeJob latest;
					if( !key.empty() && Bakes().LatestJob( key, s.name, latest ) ) o.set( "job", JobJson( latest ) );
					list.push_back( o );
				}
				result.set( "ok", Bool( true ) );
				result.set( "scene", Str( snapshot.scene.string() ) );
				result.set( "unsavedChanges", Bool( snapshot.unsavedChanges ) );
				result.set( "liveSupported", Bool( controller != nullptr ) );
				result.set( "simulations", list );
				return true;
			}

			//------------------------------------------------------------------
			// sim_cache {} -> {ok, scene, cacheBase, baseFromEnvironment, sceneCacheDir?,
			//                  trustedRoots, simulations:[{name,cacheDir,defaultLocation,exists,accepted,reason?}]}
			//   Where the caches live and whether the writer would accept each
			//   location (nothing is created).  Read-only.
			//------------------------------------------------------------------
			if( m == "sim_cache" ) {
				Simulation::BakeSceneSnapshot snapshot;
				std::string error;
				if( !AgentSimulationSnapshot( job, controller, snapshot, error ) ) return Refuse( result, error );
				namespace fs = std::filesystem;
				const fs::path base = Simulation::DefaultSimulationCacheDirectory();
				result.set( "ok", Bool( true ) );
				result.set( "scene", Str( snapshot.scene.string() ) );
				result.set( "cacheBase", Str( base.string() ) );
				const char* env = std::getenv( "RISE_SIMULATION_CACHE_DIR" );
				result.set( "baseFromEnvironment", Bool( env && *env && fs::path( env ).is_absolute() ) );
				if( !snapshot.scene.empty() ) {
					std::string e;
					const fs::path one = Simulation::DefaultSimulationCachePath( snapshot.scene, "x", e );
					if( !one.empty() ) result.set( "sceneCacheDir", Str( one.parent_path().string() ) );
				}
				JsonValue roots = JsonValue::MakeArray();
				for( const auto& r : Simulation::TrustedCacheRoots() ) roots.push_back( Str( r.string() ) );
				result.set( "trustedRoots", roots );
				JsonValue list = JsonValue::MakeArray();
				for( const auto& s : snapshot.simulations ) {
					JsonValue o = JsonValue::MakeObject();
					o.set( "name", Str( s.name ) );
					o.set( "keyword", Str( s.keyword ) );
					o.set( "cacheDir", Str( s.cacheDir.string() ) );
					const std::string dir = s.cacheDir.lexically_normal().string(), root = base.lexically_normal().string();
					o.set( "defaultLocation", Bool( dir.size() > root.size() && dir.compare( 0, root.size(), root ) == 0 ) );
					std::error_code ec;
					o.set( "exists", Bool( fs::exists( s.cacheDir, ec ) ) );
					std::string why;
					const bool accepted = Simulation::CheckCacheLocation( s.cacheDir, false, why );
					o.set( "accepted", Bool( accepted ) );
					if( !accepted ) o.set( "reason", Str( why ) );
					list.push_back( o );
				}
				result.set( "simulations", list );
				result.set( "note", Str( "A refused location needs a trusted root: trust it in the app's Simulations panel or "
					"list it in RISE_SIMULATION_TRUSTED_ROOTS. Set `cache` on a simulation chunk (propose_patch) to move one cache." ) );
				return true;
			}

			//------------------------------------------------------------------
			// sim_param_get {simulation, param?} -> {ok, simulation, keyword, solver,
			//   parameters:[{name,type,default?,modeDefaults?,minimum?,maximum?,values?,units,doc,keyed,set,value}],
			//   chunkFields:[{name,kind,repeatable,values?,doc,occurrences:[...]}]}
			//   The solver's parameter schema with this chunk's current values.
			//   `set` is false when the chunk does not state the parameter (the
			//   default applies; `value` is then the default text).  With `param`,
			//   only that entry.  Read-only.
			//------------------------------------------------------------------
			if( m == "sim_param_get" ) {
				std::string simulation, param;
				if( ParseString( params, "simulation", simulation, errorMessage ) < 0 ||
					ParseString( params, "param", param, errorMessage ) < 0 ) return false;
				if( simulation.empty() ) { errorMessage = "Invalid params: 'simulation' (string) is required"; return false; }
				Simulation::BakeSceneSnapshot snapshot;
				std::string error;
				if( !AgentSimulationSnapshot( job, controller, snapshot, error ) ) return Refuse( result, error );
				const Simulation::SceneSimulationStatus* sim = FindSimulation( snapshot, simulation, error );
				if( !sim ) return Refuse( result, error );
				const Simulation::ParameterSchema* schema = sim->solverId.empty() ? nullptr : Simulation::FindSolverSchema( sim->solverId );
				if( !schema )
					return Refuse( result, "`" + simulation + "` is a " + sim->keyword + ( sim->solverId.empty() ?
						" (no solver, so no parameter schema); read it with read_document" :
						" whose solver registered no parameter schema" ) );
				const RISE::Cst::Document* doc = job.HasRetainedCstDocument() ? job.GetCstDocument() : nullptr;
				RISE::Cst::NodeRef chunk;
				if( doc ) {
					const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, simulation, nullptr, "simulation", false );
					if( id != 0 ) chunk = RISE::Cst::DocResolveNodeId( *doc, id );
				}
				JsonValue plist = JsonValue::MakeArray(), flist = JsonValue::MakeArray();
				bool matched = param.empty();
				for( const auto& p : schema->parameters ) {
					if( !param.empty() && p.name != param ) continue;
					matched = true;
					JsonValue o = JsonValue::MakeObject();
					o.set( "name", Str( p.name ) );
					o.set( "type", Str( SchemaTypeName( p.type ) ) );
					if( !p.defaultText.empty() ) o.set( "default", Str( p.defaultText ) );
					if( !p.modeDefaults.empty() ) o.set( "modeDefaults", Str( p.modeDefaults ) );
					if( p.hasRange ) { o.set( "minimum", Num( p.minimum ) ); o.set( "maximum", Num( p.maximum ) ); }
					if( !p.values.empty() ) o.set( "values", StringArray( p.values ) );
					o.set( "units", Str( p.units ) );
					o.set( "doc", Str( p.doc ) );
					o.set( "keyed", Bool( p.keyed ) );
					bool present = false;
					std::string value;
					if( chunk ) value = Trimmed( RISE::Cst::ParamValueAsParsed( chunk, p.name, &present ) );
					o.set( "set", Bool( present ) );
					o.set( "value", present ? Str( value ) : Str( p.defaultText ) );
					plist.push_back( o );
				}
				for( const auto& f : schema->chunkFields ) {
					if( !param.empty() && f.name != param ) continue;
					matched = true;
					JsonValue o = JsonValue::MakeObject();
					o.set( "name", Str( f.name ) );
					o.set( "kind", Str( f.kind == Simulation::SchemaChunkField::Kind::ObjectReference ? "object" : "text" ) );
					o.set( "repeatable", Bool( f.repeatable ) );
					if( !f.values.empty() ) o.set( "values", StringArray( f.values ) );
					o.set( "doc", Str( f.doc ) );
					JsonValue occ = JsonValue::MakeArray();
					if( chunk ) {
						const int n = RISE::Cst::ParamOccurrenceCount( chunk, f.name );
						for( int i = 0; i < n; ++i ) occ.push_back( Str( Trimmed( RISE::Cst::ParamValueAtOccurrence( chunk, f.name, i ) ) ) );
					}
					o.set( "occurrences", occ );
					flist.push_back( o );
				}
				if( !matched ) {
					std::string names;
					for( const auto& p : schema->parameters ) names += ( names.empty() ? "" : ", " ) + p.name;
					for( const auto& f : schema->chunkFields ) names += ( names.empty() ? "" : ", " ) + f.name;
					return Refuse( result, "`" + sim->solverId + "` has no parameter `" + param + "`; it has: " + names );
				}
				result.set( "ok", Bool( true ) );
				result.set( "simulation", Str( simulation ) );
				result.set( "keyword", Str( sim->keyword ) );
				result.set( "solver", Str( sim->solverId ) );
				result.set( "state", Str( Simulation::SceneSimulationStateName( sim->state ) ) );
				result.set( "parameters", plist );
				result.set( "chunkFields", flist );
				return true;
			}

			//------------------------------------------------------------------
			// sim_bake {simulation? | all?, staleOnly?, force?} -> {ok, jobId, jobIds, jobs}
			//------------------------------------------------------------------
			if( m == "sim_bake" ) {
				std::string simulation;
				bool all = false, staleOnly = false, force = false;
				if( ParseString( params, "simulation", simulation, errorMessage ) < 0 ||
					ParseBool( params, "all", all, errorMessage ) < 0 ||
					ParseBool( params, "staleOnly", staleOnly, errorMessage ) < 0 ||
					ParseBool( params, "force", force, errorMessage ) < 0 ) return false;
				if( simulation.empty() == !all ) {
					errorMessage = "Invalid params: give exactly one of 'simulation' (a name) or 'all':true";
					return false;
				}
				Simulation::BakeSceneSnapshot snapshot;
				std::string error;
				if( !AgentSimulationSnapshot( job, controller, snapshot, error ) ) return Refuse( result, error );
				Simulation::BakeRequest request;
				request.all = all;
				request.staleOnly = staleOnly;
				request.force = force;
				if( !all ) request.simulations.push_back( simulation );
				std::vector<std::uint64_t> ids;
				if( !Bakes().RequestBake( snapshot, request, ids, error ) ) return Refuse( result, error );
				const std::string key = SimulationBakeManager::SceneKey( snapshot.scene );
				std::uint64_t primary = ids.empty() ? 0 : ids.back();
				BakeJob named;
				if( !all && Bakes().LatestJob( key, simulation, named ) ) primary = named.id;
				JsonValue idList = JsonValue::MakeArray(), jobs = JsonValue::MakeArray();
				for( const auto id : ids ) {
					idList.push_back( Num( static_cast<double>( id ) ) );
					BakeJob j;
					if( Bakes().Job( id, j ) ) jobs.push_back( JobJson( j ) );
				}
				result.set( "ok", Bool( true ) );
				result.set( "jobId", Num( static_cast<double>( primary ) ) );
				result.set( "jobIds", idList );
				result.set( "jobs", jobs );
				return true;
			}

			//------------------------------------------------------------------
			// sim_status {jobId?} -> {found, job} | {jobs:[...]}
			//------------------------------------------------------------------
			if( m == "sim_status" ) {
				std::uint64_t id = 0;
				const int present = ParseJobId( params, id, errorMessage );
				if( present < 0 ) return false;
				if( present ) {
					BakeJob j;
					const bool found = Bakes().Job( id, j );
					result.set( "found", Bool( found ) );
					if( found ) result.set( "job", JobJson( j ) );
					return true;
				}
				Simulation::BakeSceneSnapshot snapshot;
				std::string error;
				const std::string key = AgentSimulationSnapshot( job, controller, snapshot, error ) ?
					SimulationBakeManager::SceneKey( snapshot.scene ) : std::string();
				JsonValue jobs = JsonValue::MakeArray();
				if( !key.empty() ) for( const auto& j : Bakes().Jobs( key ) ) jobs.push_back( JobJson( j ) );
				result.set( "jobs", jobs );
				return true;
			}

			//------------------------------------------------------------------
			// sim_wait {jobId, timeoutMs?} -> {completed, found, job?}
			//   timeoutMs default 5000, clamped [0,60000] (render_wait's bound).
			//------------------------------------------------------------------
			if( m == "sim_wait" ) {
				std::uint64_t id = 0;
				const int present = ParseJobId( params, id, errorMessage );
				if( present < 0 ) return false;
				if( !present ) { errorMessage = "Invalid params: 'jobId' (number) is required"; return false; }
				double timeout = 5000;
				if( ParseNumber( params, "timeoutMs", timeout, errorMessage ) < 0 ) return false;
				timeout = std::min( 60000.0, std::max( 0.0, timeout ) );
				BakeJob j;
				const bool completed = Bakes().Wait( id, static_cast<unsigned>( timeout ), j );
				const bool found = Bakes().Job( id, j );
				result.set( "completed", Bool( completed ) );
				result.set( "found", Bool( found ) );
				if( found ) result.set( "job", JobJson( j ) );
				return true;
			}

			//------------------------------------------------------------------
			// sim_cancel {jobId? | simulation?, kill?} -> {cancelled, found, job?, message?}
			//   Queued: cancelled.  Running: stops at its next checkpoint and ends
			//   stopped and resumable (sim_bake continues it).  kill:true ends it
			//   at once without a checkpoint.
			//------------------------------------------------------------------
			if( m == "sim_cancel" ) {
				std::uint64_t id = 0;
				std::string simulation;
				bool kill = false;
				const int idPresent = ParseJobId( params, id, errorMessage );
				if( idPresent < 0 || ParseString( params, "simulation", simulation, errorMessage ) < 0 ||
					ParseBool( params, "kill", kill, errorMessage ) < 0 ) return false;
				if( !idPresent && simulation.empty() ) {
					errorMessage = "Invalid params: give 'jobId' or 'simulation'";
					return false;
				}
				std::string error;
				bool ok = false;
				if( idPresent ) ok = Bakes().Stop( id, kill, error );
				else {
					Simulation::BakeSceneSnapshot snapshot;
					if( !AgentSimulationSnapshot( job, controller, snapshot, error ) ) return Refuse( result, error );
					ok = Bakes().StopSimulation( SimulationBakeManager::SceneKey( snapshot.scene ), simulation, kill, &id, error );
				}
				BakeJob j;
				const bool found = id != 0 && Bakes().Job( id, j );
				result.set( "cancelled", Bool( ok ) );
				result.set( "found", Bool( found ) );
				if( found ) result.set( "job", JobJson( j ) );
				if( !ok ) result.set( "message", Str( error ) );
				return true;
			}

			//------------------------------------------------------------------
			// Live control: only with an in-app viewport.
			//------------------------------------------------------------------
			if( m == "sim_live_start" || m == "sim_live_stop" ) {
				std::string simulation;
				if( ParseString( params, "simulation", simulation, errorMessage ) < 0 ) return false;
				if( simulation.empty() ) { errorMessage = "Invalid params: 'simulation' (string) is required"; return false; }
				double fps = 0;
				if( ParseNumber( params, "fps", fps, errorMessage ) < 0 ) return false;
				if( !controller ) return Unsupported( result, m );
				std::string error;
				bool ok;
				if( m == "sim_live_start" ) {
					ok = controller->LiveSimulationAttachSimulation( simulation, error ) &&
						controller->PlaybackPlay( fps, /*loop=*/false );
				} else {
					controller->PlaybackPause();
					ok = controller->LiveSimulationDetachSimulation( simulation, error );
				}
				result.set( "ok", Bool( ok ) );
				result.set( "supported", Bool( true ) );
				if( !ok ) result.set( "message", Str( error.empty() ? std::string( "playback did not start" ) : error ) );
				return true;
			}
			if( m == "sim_seek" ) {
				double t = 0;
				const int present = ParseNumber( params, "time", t, errorMessage );
				if( present < 0 ) return false;
				if( !present ) { errorMessage = "Invalid params: 'time' (number) is required"; return false; }
				if( !controller ) return Unsupported( result, m );
				const bool ok = controller->PlaybackSeek( t );
				result.set( "ok", Bool( ok ) );
				result.set( "supported", Bool( true ) );
				result.set( "time", Num( controller->Editor().LastSceneTime() ) );
				if( !ok ) result.set( "message", Str( "the viewport refused the seek (a render owns the scene?)" ) );
				return true;
			}

			errorCode = -32601;
			errorMessage = "Method not found: " + m;
			return false;
		}
	}
}
