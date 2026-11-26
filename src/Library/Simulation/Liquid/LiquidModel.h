//////////////////////////////////////////////////////////////////////
//
//  LiquidModel.h - The liquid simulation (simulation M7): state, one time
//  step, the time-step limits, and the outputs (surface mesh, fields,
//  marker particles) of the `liquid.mac` solver.
//
//  One step of length dt (operator splitting; docs/SIMULATION_M7A_RESULTS.md):
//    1. extrapolate the velocity from the liquid's faces into the air;
//    2. advect the marker particles (when enabled) and the level set
//       (WENO5 + SSP-RK3) with that velocity;
//    3. advect the velocity (the configured momentum scheme);
//    4. correct the level set from escaped particles, carry it into solids,
//       reinitialise it, correct it again, adjust radii and reseed;
//    5. shift it so the liquid volume equals the initial one (optional;
//       the shift is reported);
//    6. add gravity; project with the new interface (ghost-fluid free
//       surface, cut-cell solids).
//  With the interface advanced by the old velocity and the velocity by the
//  new interface, the linearised free-surface coupling is the symplectic
//  Euler scheme: no amplitude error, phase error O((omega dt)^2).
//
//  Time step.  Each segment of length T (an output interval or an imposed
//  macro step) is split into n = ceil(T / dt_max) equal steps with
//      dt_max = min(cfl h / (|u|_max + |g| T),  wave_cfl h / sqrt(|g| D))
//  (advective CFL with the speed the liquid can gain over the segment; a CFL
//  on the fastest linear gravity wave, the shallow-water speed sqrt(g D) with
//  D the domain's extent along g, which bounds sqrt(g tanh(kd)/k) for every
//  depth d <= D and wavenumber k).  dt then scales with h, so the scheme's
//  O(dt^2) phase error is O(h^2).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_MODEL_H
#define RISE_SIMULATION_LIQUID_MODEL_H

#include "LiquidFlow.h"
#include "LiquidLevelSet.h"
#include "../SimulationGrid.h"
#include "../SimulationMesh.h"
#include "../SimulationPoints.h"

#include <functional>
#include <memory>
#include <string>

namespace RISE { namespace Simulation { namespace Liquid {

struct SolverOptions {
	MomentumScheme momentum = MomentumScheme::Weno5;
	int momentumStages = 2;          // 1: advect then project; 2: Heun with a projection per stage
	ReinitScheme reinit = ReinitScheme::Sussman;
	int reinitIterations = 1;
	double reinitThreshold = 0.1;  // reinitialise only when | |grad phi| - 1 | exceeds this within 3 cells (0: every step)
	double reinitCfl = 0.3;
	bool particles = true;           // measured: half the transport error for 5% of the step (docs, section 2)
	ParticleOptions particle;
	bool volumeCorrection = true;
	double cfl = 0.5;
	double waveCfl = 0.5;
	double speedLimit = 3;             // faces faster than this x sqrt(2 |g| D) are clamped (a safeguard; counted); 0: off
	double minFaceFraction = 0.5;      // faces less open than this are closed (narrow cut faces jet and diverge; docs section 2)
	int extrapolationLayers = 6;          // momentum field: WENO5 reaches 3 cells beyond the liquid, plus the CFL travel
	int interfaceExtrapolationLayers = 4; // interface field: the cells within reach of the surface's stencils
	int linearExtrapolationLayers = 1;
	PressureOptions pressure;
};

//! What a run simulates (everything but the numerics).
struct LiquidSetup {
	Grid grid;
	V3 gravity{{0, -9.81, 0}};
	double density = 1000;            // kg/m^3 (pressure output only: the flow is density-independent)
	bool openTop = false;             // +y boundary open (p = 0), else a free-slip wall
	std::vector<Shape> solids;        // static
	std::vector<Shape> liquid;        // initial liquid (union)
	V3 initialVelocity{{0, 0, 0}};
};

struct StepStats {
	double dt = 0;
	PressureStats pressure;
	double volumeShift = 0;           // m
	std::size_t escaped = 0;
	bool reinitialised = false;
	std::size_t clamped = 0;          // faces whose speed the safeguard limited this step
	// Wall seconds per phase: extrapolation, level set (advection + reinit +
	// volume), particles, momentum advection, pressure (assembly + solve + update).
	double extrapolate = 0, levelSet = 0, particles = 0, momentum = 0, project = 0;
};

class LiquidModel {
public:
	bool Initialize(const LiquidSetup& setup, const SolverOptions& options, unsigned threads, std::string& error);
	//! dt_max for a segment of length T (LiquidModel.h header comment).
	double MaxStep(double segment) const;
	bool Step(double dt, StepStats& stats, std::string& error);
	//! Projects the current velocity (no gravity, no time step): for a
	//! velocity set by hand after Initialize.
	bool ProjectState(std::string& error);
	//! Replaces the level set (a validation case's own initial interface):
	//! the target volume and the particles follow it; the velocity is projected.
	bool SetLevelSet(std::vector<double> levelSet, std::string& error);
	//! Advances by `duration` in segments of at most `segment`, each split into
	//! ceil(length / MaxStep(length)) equal steps (as the solver does);
	//! `after` (may be empty) is called after every step with the time.
	bool Run(double duration, double segment, const std::function<void(double, const StepStats&)>& after, std::string& error);
	double Time() const { return time_; }
	void SetThreads(unsigned threads);

	const Grid& GridSpec() const { return setup_.grid; }
	const LiquidSetup& Setup() const { return setup_; }
	const SolverOptions& Options() const { return options_; }
	//! Physical pressure (Pa) of the last projection.
	double Pressure(std::size_t cell) const { return lastDt_ > 0 ? q_[cell]*setup_.density*setup_.grid.h/lastDt_ : 0.0; }
	double MaxSpeed() const;
	double Volume() const;
	double TargetVolume() const { return targetVolume_; }

	// State (checkpoints).
	std::vector<double> phi;
	MacField u;
	FaceMask valid;
	std::vector<double> q_;
	double lastDt_ = 0;
	std::uint64_t steps = 0;
	double targetVolume_ = 0;
	MarkerParticles particles;

	// Static data derived from the setup.
	std::vector<double> solidNodes, openCells;
	std::array<std::vector<double>,3> fraction;

	// Outputs.
	//! The liquid surface: marching cubes on phi with the solids carved out
	//! half a cell inside their surface and the domain walls closing it.
	bool Surface(MeshFrame& frame, std::string& error) const;
	//! phi (m), pressure (Pa) and the cell-centred velocity (m/s) on the cells.
	bool Fields(GridFrame& frame, std::string& error) const;
	bool Points(PointsFrame& frame, std::string& error) const;

	Pool* ThreadPool() const { return pool_.get(); }
private:
	LiquidSetup setup_;
	SolverOptions options_;
	std::unique_ptr<Pool> pool_;
	double depth_ = 1;    // the domain's extent along gravity (wave limit)
	double time_ = 0;     // accumulated by Run
};

//! Marching cubes on a sampled scalar field with a face-consistent
//! triangulation: each cube face's crossing edges are joined by segments,
//! an ambiguous face (alternating corner signs) resolved by the asymptotic
//! decider (Nielson and Hamann 1991), which depends only on the face's four
//! values, so adjacent cubes agree and the surface has no cracks; the
//! segments of a cube chain into closed loops; a triangle is one triangle,
//! a longer loop a fan around its centroid (a fan from a loop vertex could
//! put a chord on a cube face that the neighbouring cube also used: a
//! non-manifold edge).  Edge vertices sit on the sample edges (shared by
//! every cube that uses them); centroid vertices follow them.  `values` has
//! dimensions d (x fastest); sample (i, j, k) is at origin + h (i, j, k).
//! Triangles are counter-clockwise seen from the positive side.
struct IsoSurface {
	std::vector<double> positions;              // 3 per vertex
	std::vector<std::uint32_t> triangles;
	std::size_t edgeVertices = 0;               // vertices [0, edgeVertices) are on sample edges
	std::vector<std::uint64_t> edges;           // per edge vertex: 3 * sample index + axis
	std::vector<double> t;                      // per edge vertex: position along its edge
	std::vector<std::uint32_t> loopStart, loopMembers;   // per centroid vertex: its loop's edge vertices
};
void MarchingCubes(const std::array<int,3>& d, const std::vector<double>& values, const V3& origin, double h,
	IsoSurface& out, Pool* pool);

} } }

#endif
