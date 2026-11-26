#include "pch.h"
#include "AgentSimulation.h"

#include "../Interfaces/IJobPriv.h"
#include "../SceneEditor/SceneEditController.h"
#include "../SceneEditor/FileIdentity.h"
#include "../SceneEditor/LiveSimulation.h"
#include "../Simulation/SimulationBakeManager.h"
#include "../Simulation/BuiltinSolvers.h"
#include "../Cst/Cst.h"

#include <cmath>
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
			return m == "sim_list" || m == "sim_status" || m == "sim_wait" || IsAgentSimulationControlVerb( m );
		}

		bool IsAgentSimulationControlVerb( const std::string& m )
		{
			return m == "sim_bake" || m == "sim_cancel" || m == "sim_live_start" || m == "sim_live_stop" || m == "sim_seek";
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
