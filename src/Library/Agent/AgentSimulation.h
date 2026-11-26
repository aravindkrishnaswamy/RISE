// Simulation M5: the agent's simulation verbs (docs/SIMULATION_M5A_RESULTS.md,
// docs/gui/MCP_TOOL_SURFACE.md §4.4a).  AgentRpc dispatches them here.
//
//   sim_list   {}                                      read-safe
//   sim_status {jobId?}                                read-safe
//   sim_wait   {jobId, timeoutMs?}                     read-safe; timeoutMs default 5000, clamped [0,60000]
//   sim_bake   {simulation? | all?, staleOnly?, force?} Commit autonomy, Owner session
//   sim_cancel {jobId? | simulation?, kill?}           Commit autonomy, Owner session
//   sim_live_start {simulation, fps?}                  live controller only
//   sim_live_stop  {simulation}                        live controller only
//   sim_seek       {time}                              live controller only
//
// Baking goes through the process-wide SimulationBakeManager, so it works
// the same in a GUI-hosted session and in headless `rise --agent-stdio` /
// `--mcp` (the worker is a separate rise-sim process).  The live verbs need
// an in-app viewport: headless they answer {ok:false, supported:false}.
#ifndef RISE_AGENT_SIMULATION_H
#define RISE_AGENT_SIMULATION_H

#include "Json.h"
#include <string>

namespace RISE
{
	class IJobPriv;
	class SceneEditController;
	namespace Simulation { struct BakeSceneSnapshot; }

	namespace Agent
	{
		//! True for every sim_* verb above.
		bool IsAgentSimulationVerb( const std::string& method );
		//! True for the verbs that change something outside the document
		//! (start or stop a bake, drive the live viewport): sim_bake,
		//! sim_cancel, sim_live_start, sim_live_stop, sim_seek.
		bool IsAgentSimulationControlVerb( const std::string& method );

		//! Runs a sim_* verb.  `controller` is the session's live controller,
		//! or null (headless).  False with `errorCode` (a JSON-RPC code) and
		//! `errorMessage` for malformed params; a refusal the caller can act on
		//! (save first, unknown simulation, nothing to resume) is a SUCCESS
		//! result with ok:false and a message, like render's ok:false.
		bool HandleAgentSimulationVerb( const std::string& method, const JsonValue& params,
			IJobPriv& job, SceneEditController* controller,
			JsonValue& result, int& errorCode, std::string& errorMessage );

		//! What a session tells the bake manager about its document: the
		//! Job's scene file, unsaved changes (the serialized Document against
		//! the file's bytes), and its simulations.
		bool AgentSimulationSnapshot( IJobPriv& job, SceneEditController* controller,
			Simulation::BakeSceneSnapshot& snapshot, std::string& error );
	}
}

#endif
