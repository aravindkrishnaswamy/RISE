//////////////////////////////////////////////////////////////////////
//
//  LiveSimulation.h - Simulation M3c: simulated deforming geometry in the
//    interactive viewport.  Two pieces the SceneEditController owns:
//
//    LiveSimulation - one in-process simulation (an InProcessHost) whose
//      in-memory publication is bound to a simulated_mesh_geometry in
//      place of its baked cache.  The solver steps on the host's worker
//      thread and touches no scene data; the render thread reads the
//      publication's immutable snapshots when it installs a frame.
//
//    PlaybackClock - play / pause / seek of scene time at a frame rate on
//      a controller-owned thread.  Each displayed frame goes through the
//      controller's ordinary time-scrub path (cancel, park, mutate,
//      install, kick).  When the viewport renders slower than the clock,
//      frames are coalesced (the next frame waits for a completed pass)
//      and counted as dropped; with live simulations the clock never runs
//      ahead of the latest published frame.
//
//  Threading model and API: docs/SIMULATION_M3C_RESULTS.md.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCENE_EDITOR_LIVE_SIMULATION_
#define RISE_SCENE_EDITOR_LIVE_SIMULATION_

#include "../Simulation/SimulatedMeshSource.h"
#include "../Simulation/SimulationSolver.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RISE
{
	namespace Simulation { class InProcessHost; }
	class IJobPriv;

	namespace Implementation
	{
		class SimulatedMeshGeometry;

		//! One live simulation, as LiveSimulation::GetStatus reports it.
		struct LiveSimulationStatus
		{
			bool			playing = false;		//!< the solver is stepping on its worker thread
			bool			finished = false;		//!< every output frame is published
			bool			failed = false;
			bool			recording = false;
			double			simulatedTime = 0;
			std::int64_t	latestFrame = -1;		//!< last frame of the run published from the first
			double			latestSceneTime = 0;	//!< its scene time (valid when latestFrame >= first)
			double			firstSceneTime = 0, lastSceneTime = 0;	//!< the run's whole span
			std::uint64_t	framesPublished = 0, framesTotal = 0;
			std::uint64_t	restarts = 0;
			std::string		solverId, cacheKey, error;
		};

		//! Installs every simulated mesh some object of the scene renders at
		//! scene time t, with no shutter (the viewport renders stills), and
		//! invalidates the top-level structure.  The caller parks the
		//! render (SceneEditController's cancel-wait-mutate-kick).  Returns
		//! the number of meshes; false (with the first error) if one failed,
		//! its previous frame kept.
		bool InstallSimulatedMeshesAtTime( IJobPriv& job, double t, unsigned int& meshes, std::string& error );

		class LiveSimulation
		{
		public:
			//! Builds the solver from the registry, configures it (scene
			//! inputs are M4: none here), starts it and publishes its first
			//! frame on the calling thread, and opens a live source of the
			//! geometry's topology.  Touches no scene data: the caller binds
			//! TakeBindSource() under the render park.  Refuses a solver
			//! that is not interactive or whose output is not the mesh's.
			static std::unique_ptr<LiveSimulation> Create(
				const std::string& geometryName, SimulatedMeshGeometry& geometry,
				const std::string& solverId, const std::vector<unsigned char>& parameters,
				std::string& error );
			//! Simulation M4: a live run of the scene's simulation `simulation`
			//! (its solver and parameters as declared, its scene inputs captured
			//! detached from the scene by the caller), bound to the consumer
			//! `geometry`, which may have no source yet (its producer was never
			//! baked): then the run's first frame gives it its topology.
			//! `options` is the consumer's binding.  `fingerprint` identifies the
			//! configuration (parameters and input identities), so the controller
			//! can tell when a scene edit changed it.
			static std::unique_ptr<LiveSimulation> CreateForSimulation(
				const std::string& simulation, const std::string& geometryName, SimulatedMeshGeometry& geometry,
				const std::string& solverId, const std::vector<unsigned char>& parameters,
				std::shared_ptr<const Simulation::ISceneInputs> inputs, const Simulation::MeshSourceOptions& options,
				const std::string& fingerprint, std::string& error );
			//! A live run of a scene simulation that publishes transforms (a
			//! transform.v1 output, e.g. rigid.bodies) and has no mesh: it drives
			//! the transform_source objects that follow its bodies
			//! (Job::SetLiveTransformProvider).  `consumerName` (the first such
			//! object) is the name the run is addressed by, as a mesh run is by its
			//! geometry's.  Same configuration capture as CreateForSimulation.
			static std::unique_ptr<LiveSimulation> CreateForTransformSimulation(
				const std::string& simulation, const std::string& consumerName,
				const std::string& solverId, const std::vector<unsigned char>& parameters,
				std::shared_ptr<const Simulation::ISceneInputs> inputs,
				const std::string& fingerprint, std::string& error );
			~LiveSimulation();	//!< stops and joins the solver thread

			//! True for a transform run (no mesh: Geometry() must not be called).
			bool IsTransform() const { return m_transform; }
			//! The run's current publication, following restarts; null once the
			//! run is gone.  Thread-safe and cheap (a transform run only).
			std::function<std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot>()> TransformProvider() const;
			//! Scene time of the run's first frame (a transform run).
			double FirstSceneTime() const;

			//! The scene simulation this run plays (empty for an API attach).
			const std::string& SimulationName() const { return m_simulation; }
			const std::string& Fingerprint() const { return m_fingerprint; }

			const std::string& GeometryName() const { return m_name; }
			SimulatedMeshGeometry& Geometry() const { return *m_geometry; }

			//! The live source to bind (from Create or PrepareRestart).
			std::unique_ptr<Simulation::MeshCacheSource> TakeBindSource();
			//! The geometry's baked source while live; restored on detach.
			void KeepOriginalSource( std::unique_ptr<Simulation::MeshCacheSource> source );
			std::unique_ptr<Simulation::MeshCacheSource> TakeOriginalSource();

			bool Play( std::string& error );	//!< solver steps on the host's worker thread
			void Pause();						//!< interrupts the current step at a safe point
			bool WaitIdle( std::string& error );

			//! A run from the initial state, with new parameters (a new cache
			//! key) or a disk recording (`record`: InProcessHost::Record) if
			//! given.  The new run is built here, off every scene lock; the
			//! current one keeps serving the geometry until CommitRestart
			//! (after the caller rebound TakeBindSource) or AbortRestart.
			bool PrepareRestart( const std::vector<unsigned char>* parameters,
				const std::filesystem::path* record, std::string& error );
			void CommitRestart();
			void AbortRestart();

			//! Scene time of the latest frame published contiguously from the
			//! first, and whether the run has published all of them.  False
			//! before any frame.  Thread-safe (the playback clock calls it).
			bool Latest( double& sceneTime, bool& finished ) const;
			LiveSimulationStatus GetStatus() const;

		private:
			struct Feed;
			struct Run;
			LiveSimulation() = default;
			static bool StartTransformRun( const std::string& solverId, const std::vector<unsigned char>& parameters,
				const std::filesystem::path* record, Run& run, std::string& error );
			static bool StartRun( const std::string& solverId, const std::vector<unsigned char>& parameters,
				const Simulation::MeshSourceOptions& options, const Simulation::MeshCacheSource* topology,
				const std::filesystem::path* record, Run& run, std::string& error );

			std::string						m_name, m_solverId;
			SimulatedMeshGeometry*			m_geometry = nullptr;	//!< counted reference
			Simulation::MeshSourceOptions	m_options;
			std::unique_ptr<Simulation::MeshCacheSource> m_original;
			mutable std::mutex				m_mutex;	//!< guards the run pointers (swapped by restart)
			std::unique_ptr<Run>			m_run, m_pending;
			std::uint64_t					m_restarts = 0;
			// M4: the scene simulation, its captured scene inputs (shared by
			// restarts; they outlive every solver configured with them) and
			// the configuration fingerprint.
			std::string						m_simulation, m_fingerprint;
			std::shared_ptr<const Simulation::ISceneInputs> m_inputs;
			// A transform run: the publication a Job follows across restarts.
			struct Hub;
			bool							m_transform = false;
			std::shared_ptr<Hub>			m_hub;
		};

		//! The playback clock, as PlaybackClock::GetStatus reports it.
		struct PlaybackStatus
		{
			bool			playing = false;
			bool			live = false;			//!< following a live simulation
			bool			waitingForSolver = false;
			double			time = 0;				//!< last scene time shown
			double			start = 0, end = 0, fps = 0;
			//! M5c: scene seconds per wall-clock second (1 = real time).  A
			//! frame-stepping playback shows frame k at wall time k / (fps x
			//! timeScale) -- SceneEditController::PlaybackPlay's rule.
			double			timeScale = 1;
			bool			loop = false;
			std::uint64_t	framesShown = 0;		//!< applied through the scrub path
			std::uint64_t	framesDropped = 0;		//!< clock frames never shown (render slower than the clock)
			std::uint64_t	solverWaits = 0;		//!< ticks the clock held for the solver
			std::uint64_t	passesCompleted = 0;	//!< viewport passes finished since Play
			double			wallSeconds = 0;		//!< since Play
		};

		class PlaybackClock
		{
		public:
			struct Hooks
			{
				std::function<bool()>		begin;		//!< open the scrub composite
				std::function<bool(double)>	apply;		//!< show scene time t (the scrub path)
				std::function<void()>		end;		//!< close it
				std::function<std::uint64_t()> completedPasses;	//!< viewport passes finished uncancelled
				//! The latest live frame's scene time (false: no live simulation),
				//! and whether every live run has finished.
				std::function<bool( double&, bool& )> liveLimit;
				std::function<void( bool )> livePlay;	//!< start / pause the live solvers
			};

			explicit PlaybackClock( Hooks hooks );
			~PlaybackClock();	//!< Shutdown

			//! Plays [start, end] from `from` on a grid of `fps` frames per
			//! scene second; scene time advances `timeScale` scene seconds per
			//! wall-clock second (1: real time), quantized to the grid.  `loop`
			//! wraps; otherwise playback stops at the end.  A second Play while
			//! playing updates rate and range.
			bool Play( double start, double end, double from, double fps, bool loop, std::string& error,
				double timeScale = 1.0 );
			void Pause();
			//! While playing, moves the clock; false when paused (the caller
			//! scrubs directly).
			bool Seek( double t );
			PlaybackStatus GetStatus() const;
			bool Playing() const;
			bool OnClockThread() const;
			void Shutdown();

		private:
			void Loop();
			void CloseComposite();

			Hooks						m_hooks;
			mutable std::mutex			m_mutex;
			std::condition_variable		m_cv;
			std::mutex					m_applyMutex;	//!< held around each apply
			std::mutex					m_compositeMutex;
			bool						m_compositeOpen = false;
			std::thread					m_thread;
			std::atomic<std::thread::id> m_threadId;
			bool						m_quit = false;
			bool						m_playing = false;
			bool						m_resync = false;
			PlaybackStatus				m_status;
			double						m_clock = 0;
			long long					m_lastFrame = -1;
			std::uint64_t				m_passesAtApply = 0, m_passesAtPlay = 0;
			std::chrono::steady_clock::time_point m_lastTick, m_lastApply, m_playWall;
		};
	}
}

#endif
