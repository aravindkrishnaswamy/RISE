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
//    3. advect the velocity (the configured momentum scheme; WENO5 stages
//       bounded by their upwind values, LiquidFlow.h LimitedAdvectionStage);
//    4. correct the level set from escaped particles, carry it into solids,
//       reinitialise it, correct it again, adjust radii and reseed;
//    5. shift it so the liquid volume equals the initial one (optional;
//       the shift is reported);
//    6. add gravity; project with the new interface (ghost-fluid free
//       surface, cut-cell solids).
//  m7c: with viscosity, the step's last stage (before its projection) solves
//  the implicit viscous step on the advected field with gravity and the
//  previous step's pressure gradient (an incremental pressure; LiquidViscosity.h),
//  coupled bodies being unknowns of that solve; with surface tension each
//  projection imposes the Laplace jump (LiquidFlow.h PressureOptions) with the
//  curvature of the new interface, and the interface velocity is extrapolated
//  by a constant.
//  Rigid bodies (m7b, LiquidBodies.h): after step 2 every body moves to its
//  pose at the step's end (dynamic and coupled bodies drift with their
//  step-start velocity, as the interface does), its cut cells are rebuilt and
//  the level set in the cells it encloses is extended from the liquid; each
//  projection of step 6 solves the coupled bodies' velocities with the
//  pressure (their predicted velocities w* = w + dt F_ext / m in the
//  right-hand side, rho h^3 J^T M^-1 J in the operator; Heun's second stage
//  starts from w* + M^-1 I1 / 2), so the linearised body-surface coupling is
//  symplectic Euler as well.
//  Sheets coupled implicitly (m7e, LiquidSheets.h): each projection solves
//  the sheet's linearised backward-Euler step with the pressure, the liquid
//  on the faces it closes moving with it (SheetFaceMass); the sheet's new
//  velocities and positions follow, each step's impulse on it is kept for
//  its partner, and cells it swept change sides (SweepSheetCells).  m7f:
//  the level set's stencils treat the faces such a sheet closes as walls
//  (LiquidLevelSet.h LevelSetBarrier), and beside a sheet holding liquid on
//  one side the level set is the signed distance to it (PinSheetInterface).
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

#include "LiquidBodies.h"
#include "LiquidFlow.h"
#include "LiquidLevelSet.h"
#include "LiquidSheets.h"
#include "LiquidViscosity.h"
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
	double speedLimit = 3;             // valid faces faster than this x sqrt(2 |g| D) are clamped (a safeguard; counted); 0: off
	double minFaceFraction = 0;        // cut faces less open than this are closed (0: none; closing locates solids to half a cell)
	double minTransportFraction = 0.25; // cut faces less open leave each step invalid, re-extrapolated (LiquidFlow.h Project)
	int extrapolationLayers = 6;          // momentum field: WENO5 reaches 3 cells beyond the liquid, plus the CFL travel
	int interfaceExtrapolationLayers = 4; // interface field: the cells within reach of the surface's stencils
	int linearExtrapolationLayers = 1;
	PressureOptions pressure;
	// m7c (LiquidViscosity.h; the surface tension in LiquidFlow.h PressureOptions).
	double viscosityTolerance = 1e-8;   // relative residual of the viscous solve
	double viscousThetaMin = 0.05;      // smallest wall fraction of a no-slip difference
	int viscousSamples = 4;             // samples per axis of a cut control volume (viscous weights)
	double viscousCfl = 4;              // dt <= viscousCfl h^2 / nu with viscosity (0: none): the viscous-pressure splitting converges per step (docs)
	double capillaryCfl = 0.5;          // dt <= capillaryCfl sqrt(rho h^3 / (2 pi sigma)) with surface tension
	bool interpolateCurvature = true;   // the curvature at the sub-cell interface position (else the liquid cell's)
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
	//! m7c: constant dynamic viscosity (Pa s; 0: inviscid), no-slip or free-slip
	//! walls, solids and bodies for it, and the surface tension (N/m; 0: none).
	double viscosity = 0;
	bool noSlip = true;
	double surfaceTension = 0;
	//! Rigid bodies (m7b, LiquidBodies.h): their initial state (mass and
	//! moments set; a Dynamic body's externalForce is m g).
	std::vector<LiquidBody> bodies;
	//! Sheets (m7d, LiquidSheets.h): thin open surfaces as moving boundaries
	//! (preview); a Port sheet's geometry arrives through its port.
	std::vector<LiquidSheet> sheets;
};

struct StepStats {
	double dt = 0;
	PressureStats pressure;
	double volumeShift = 0;           // m
	std::size_t escaped = 0;
	bool reinitialised = false;
	std::size_t clamped = 0;          // valid faces whose speed the safeguard limited this step
	ViscosityStats viscous;           // m7c: the viscous solve (iterations 0 when off)
	std::size_t sheetCellsKept = 0;   // m7d: cells beside a sheet kept from filling across it
	std::size_t sheetParticlesRemoved = 0;   // m7d: marker particles carried through a sheet
	// Wall seconds per phase: extrapolation, level set (advection + reinit +
	// volume), particles, momentum advection, pressure (assembly + solve + update).
	double extrapolate = 0, levelSet = 0, particles = 0, momentum = 0, project = 0, bodies = 0, viscosity = 0;
};

struct IsoSurface;

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
	//! The time a step starts at (prescribed bodies; Step advances it by dt).
	void SetTime(double t) { time_ = t; }
	void SetThreads(unsigned threads);

	const Grid& GridSpec() const { return setup_.grid; }
	const LiquidSetup& Setup() const { return setup_; }
	const SolverOptions& Options() const { return options_; }
	//! Physical pressure (Pa) of the last projection.
	double Pressure(std::size_t cell) const { return lastDt_ > 0 ? q_[cell]*setup_.density*setup_.grid.h/lastDt_ : 0.0; }
	double MaxSpeed() const;
	double Volume() const;
	//! Totals of StepStats::clamped since Initialize (not checkpointed: a run's own count).
	std::uint64_t clampedFaces = 0, clampedSteps = 0;
	//! m7g, coupled sheets: totals since Initialize (not checkpointed) of the cells that turned wet in a step
	//! without a neighbour that was wet at the step's start across an open face and without a coupled sheet
	//! sweeping over them, before the guard that holds them (LiquidSheets.h BlockSheetLeaks):
	//! `sheetCrossed` those with such a neighbour across a sheet's closed face (liquid that would have passed
	//! the sheet; volume: the cells' sharp fill clamp(1/2 - phi/h, 0, 1) h^3), `sheetUnfed` the others.
	std::uint64_t sheetCrossedCells = 0, sheetUnfedCells = 0;
	//! m7h: the watertightness check of the barrier of implicitly coupled sheets whose boundary is
	//! pinned all round (a bag, the catch's cloth): a flood fill from the cells beside the sheet on
	//! its front side, through open faces and only among the cells beside the sheet's closed faces
	//! (the triangles away from the pinned rim), counts the cells beside it on its back side that
	//! it reaches: a path of open faces through the sheet itself (0: the barrier is watertight).
	//! Returns -1 when no sheet qualifies.  Diagnostic, never changes results.
	long long SheetBarrierHoles() const;
	double sheetCrossedVolume = 0;
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
	//! Rigid bodies (m7b): the current state; the hydrodynamic impulse of the
	//! last step and accumulated since ResetBodyImpulses (force and torque
	//! about the centre of mass, times time: N s, N m s).
	std::vector<LiquidBody> bodies;
	std::vector<Vec6> bodyImpulse, bodyAccumulated;
	void ResetBodyImpulses();
	//! Momentum audit (m7b): when set, each step stores the impulse the walls
	//! and static solids exerted on the liquid through the pressure.
	bool audit = false;
	V3 wallImpulse{{0, 0, 0}};
	//! m7c: the viscous impulse of the static solids and domain walls on the
	//! liquid in the last step (included in wallImpulse when auditing).
	V3 viscousWallImpulse{{0, 0, 0}};
	//! m7c: the viscous part of each body's impulse in the last step (in bodyImpulse too).
	std::vector<Vec6> bodyViscousImpulse;
	//! m7c: the curvature of the last step's interface (surface tension only).
	const std::vector<double>& Kappa() const { return kappa_; }
	//! The liquid's discrete momentum rho h^3 sum_f w_f u_f per axis, w_f the
	//! projection's kinetic-energy weight (A between liquid cells, A theta to
	//! the air), on valid faces.
	V3 FluidMomentum() const;
	//! The liquid's mass the same weights give (rho h^3 sum_f w_f over the faces of `axis`).
	double FluidMass(int axis) const;
	const BodyGeometry& Geometry() const { return geometry_; }
	//! Rebuilds the solid geometry from the bodies' current poses (after a
	//! pose was set from outside: a port, with `extrapolate` the level set in
	//! the cells they now enclose; a restored state, without: phi is restored).
	void UpdateBodyGeometry(bool extrapolate);
	//! The speed bound the time step uses for the bodies.
	double BodySpeed() const;
	//! m7d: the sheets' current state (positions and velocities at the end of
	//! the last step), the impulse the liquid exerted on each sheet's vertices
	//! since ResetSheetImpulses (3 per vertex, N s) and in the last step in total
	//! (N s per axis).  Without sheets nothing of this runs.
	std::vector<LiquidSheet> sheets;
	std::vector<std::vector<double>> sheetImpulse;
	std::vector<V3> sheetStepImpulse;
	//! m7e: per coupled sheet, the impulse on its vertices in each step since
	//! ResetSheetImpulses (3 per vertex per step, step-major).
	std::vector<std::vector<double>> sheetHistory;
	void ResetSheetImpulses();
	//! Recomputes the faces the sheets close (after their positions were set
	//! from outside: a port, a restored state).  m7e, `sweep`: the cells a coupled
	//! sheet moved over change sides (SweepSheetCells; a port, not a restore).
	void UpdateSheetGeometry(bool sweep = false);
	const std::vector<SheetCrossing>& SheetFaces() const { return sheetFaces_; }
	//! m7e: whether a sheet is coupled implicitly (with its accepted dynamics).
	bool CoupledSheets() const;
	//! m7e: after a coupled sheet's state was set from outside (a port): its
	//! elastic positions restart at x, its closed faces carry its velocity.
	void ResetSheetDynamics();

	// Static data derived from the setup.
	std::vector<double> solidNodes, openCells;
	std::array<std::vector<double>,3> fraction;

	// Outputs.
	//! The liquid surface: marching cubes on phi with the solids carved out
	//! half a cell inside their surface and the domain walls closing it.
	bool Surface(MeshFrame& frame, std::string& error) const;
	//! phi (m), pressure (Pa) and the cell-centred velocity (m/s) on the cells
	//! (the velocities of all three outputs are sampled from OutputVelocity).
	bool Fields(GridFrame& frame, std::string& error) const;
	bool Points(PointsFrame& frame, std::string& error) const;

	Pool* ThreadPool() const { return pool_.get(); }
	//! Diagnostics only (not state; empty in the solver): called with a stage's name inside Step.
	std::function<void(const char*)> stageHook;
private:
	//! The pressure options of a step's last projection (and of Initialize).
	PressureOptions FinalPressure() const;
	//! The options of a step's projection with the surface-tension jump of a
	//! stage of weight `weight` in the step's pressure (m7c; unchanged without it).
	PressureOptions WithJump(PressureOptions po, double dt, double weight) const;
	//! m7c: the implicit viscous step on `s` with the bodies' wall velocities `w`
	//! (`impulse` receives each body's viscous impulse).
	bool Viscous(MacField& s, double dt, std::vector<Vec6>& w, std::vector<Vec6>& impulse, StepStats& stats, std::string& error);
	//! m7i: the surface's vertices where a coupled sheet holds the liquid moved clear of it (Surface).
	void ClearSheets(const std::array<int,3>& d, const std::vector<double>& f, IsoSurface& iso) const;
	//! The velocity the outputs sample: u with its invalid faces extrapolated
	//! as for the interface (the field the next step moves the surface with).
	MacField OutputVelocity() const;
	//! Bodies: poses at the end of a step of length dt (Dynamic and coupled
	//! Port bodies drift with their step-start velocity; prescribed ones
	//! follow their motion; kinematic Port bodies their port velocity).
	void MoveBodies(double dt);
	//! The level set in cells a body encloses, extrapolated from the others.
	void ExtrapolateEnclosed();
	//! One projection with the bodies: their predicted velocities wStar,
	//! impulses out (coupled bodies in the solve, kinematic ones measured).
	bool ProjectBodies(MacField& s, const PressureOptions& po, const std::vector<Vec6>& wStar, std::vector<Vec6>& impulse,
		PressureStats& stats, std::string& error);
	//! m7e: the face mass of the coupled sheets (LiquidModel.cpp) from the stage's predicted face velocities `s`.
	void SheetFaceMass(const MacField& s, std::vector<std::vector<SheetBlock>>& blocks, std::vector<std::vector<double>>& rhs) const;
	//! m7e: the cells whose centre a coupled sheet swept over in the step (the crossings
	//! `before` and the current ones, paired in order along each lattice line), each with
	//! the cell beyond the old crossing on its new side.
	struct SweptCell { std::size_t cell = 0, ref = 0; double distance = 0, span = 0; };   // span: |ref - cell| (m)
	//! m7f: the level set beside a one-sided coupled sheet: the signed distance to it (cells changed).
	std::size_t PinSheetInterface(std::vector<std::uint8_t>* mask = nullptr);   // mask: the cells set (0/1 per cell)
	//! m7f: one cell's level set re-distanced from its own side's neighbours (fast marching's update).
	void Redistance(std::size_t cell);
	//! m7f: the level set's barrier from the coupled sheets' faces and the current level set.
	void BuildBarrier();
	//! m7f: whether a cell of the other sign lies within two steps across open faces.
	bool OwnInterface(std::size_t cell) const;
	//! m7e: applies the swept cells' sides (cells and particles changed, counts returned).
	std::pair<std::size_t, std::size_t> SweepSheetCells(const std::vector<SheetCrossing>& before);
	void SweptSheetCells(const std::vector<SheetCrossing>& before, std::vector<SweptCell>& swept) const;
	//! m7e: one projection with the bodies and the implicitly coupled sheets
	//! (their predicted velocities vStar; impulses out).
	bool ProjectCoupled(MacField& s, const PressureOptions& po, const std::vector<Vec6>& wStar,
		const std::vector<std::vector<double>>& vStar, std::vector<Vec6>& impulse, std::vector<std::vector<double>>& sheetImpulse,
		PressureStats& stats, std::string& error);
	std::vector<Vec6> BodyVelocities() const;
	void SetBodyVelocity(std::size_t i, const Vec6& w);
	void AuditWalls();
	bool HasSolids() const { return !setup_.solids.empty() || !bodies.empty(); }
	//! The interface field's linear layers: none with surface tension (m7c: the linear
	//! extrapolation drove a static drop's parasitic currents to the capillary speed,
	//! in 2-D without viscosity and in 3-D with it; docs/SIMULATION_M7C_RESULTS.md).
	int InterfaceLinearLayers() const { return setup_.surfaceTension > 0 ? 0 : options_.linearExtrapolationLayers; }
	// m7d sheets: the closed faces, their fractions before closing (same order), the
	// solid face velocity without bodies, and the faces whose velocity was set.
	void MoveSheets(double dt);
	void RestoreSheetFaces();
	void CloseSheetFaces();
	void ClearSheetVelocity(MacField& solid);
	void SetSheetVelocity(MacField& solid);
	void AccumulateSheetImpulse();
	//! m7g: the faces a coupled sheet closes whose liquid ends short of the sheet (a free surface
	//! between the wet cell's centre and the crossing) open for the step's projections; closed again by CloseUnwetted.
	void OpenUnwetted();
	void CloseUnwetted();
	std::vector<std::uint8_t> sheetUnwetted_;
	std::vector<std::size_t> sweptCells_;        // m7g: the cells swept by coupled sheets in the current step
	void CountSheetCrossings(const std::vector<double>& phiStart, const std::vector<SheetCrossing>& facesBefore);   // m7g: per crossing (sheetFaces_ order), 1 while opened (empty: none)
	std::vector<SheetCrossing> sheetFaces_;
	std::vector<double> sheetSaved_;
	std::vector<double> sheetClosed_;   // m7i: the fractions the closing set (A (1 - c), or 0)
	std::array<std::vector<std::uint8_t>,3> sheetWall_;   // m7i: faces left partly open that are barriers (cover >= 1/2; empty: none)
	bool SheetWall(int axis, std::size_t face) const { return !sheetWall_[std::size_t(axis)].empty() && sheetWall_[std::size_t(axis)][face]; }
	LevelSetBarrier barrier_;   // m7f: the coupled sheets' closed faces as walls of the level set
	MacField sheetSolid_;
	std::array<std::vector<std::size_t>,3> sheetTouched_;
	BodyGeometry geometry_;
	std::vector<double> kappa_;
	std::array<std::vector<std::int16_t>,3> owner_;
	MacField solidVelocity_;
	std::vector<std::uint8_t> enclosedMark_;
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
