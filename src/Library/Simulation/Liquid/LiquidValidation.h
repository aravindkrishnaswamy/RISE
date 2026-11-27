//////////////////////////////////////////////////////////////////////
//
//  LiquidValidation.h - Canonical problems of the liquid solver
//  (simulation M7) with references computed here or from closed forms,
//  shared by tests/LiquidValidationTest.cpp and the evidence the solver
//  computes at Configure (docs/SIMULATION_M7A_RESULTS.md section 4).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_VALIDATION_H
#define RISE_SIMULATION_LIQUID_VALIDATION_H

#include "LiquidModel.h"

#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Liquid {

// ------------------------------------------------------------------ interface transport (case 1)

struct TransportOptions {
	ReinitScheme reinit = ReinitScheme::Sussman;      // the solver's defaults (LiquidModel.h SolverOptions)
	int reinitIterations = 1;
	double reinitThreshold = 0.1;
	double reinitCfl = 0.3;
	bool particles = false;
	ParticleOptions particle;
	bool volumeCorrection = false;
	double cfl = 0.5;
};

struct TransportResult {
	int n = 0, steps = 0;
	double areaChange = 0;    // (A(T) - A(0)) / A(0), the numerical areas
	double areaError = 0;     // (A(T) - A_exact) / A_exact
	double l1 = 0;            // area of the symmetric difference with the exact shape / exact perimeter
	double maxShift = 0, sumShift = 0;   // volume-correction shifts, / h
	int reinitCalls = 0;
	double seconds = 0;
	std::size_t particles = 0;
};

//! Zalesak's slotted disk (radius 0.15 at (0.5, 0.75), slot 0.05 wide up to
//! y = 0.85) on the unit square, rigid rotation u = 2 pi (0.5 - y, x - 0.5),
//! one revolution (T = 1); n x n x 1 cells (the z axis one cell thick: every
//! z difference vanishes, so it is the two-dimensional scheme exactly).
TransportResult RunZalesak(int n, const TransportOptions& options, Pool* pool);
//! Single-vortex deformation (LeVeque 1996, Rider and Kothe 1998): stream
//! function psi = sin^2(pi x) sin^2(pi y) cos(pi t / T) / pi,
//! u = -d psi/dy, v = d psi/dx; circle radius 0.15 at (0.5, 0.75); the flow
//! reverses, so the exact shape at T is the initial circle.  Face velocities
//! are differences of psi at the cell corners (discretely divergence-free).
TransportResult RunVortex(int n, double period, const TransportOptions& options, Pool* pool);

//! Reinitialisation drift: phi0 = (|x - c| - R) f(x), a level set of a
//! sphere of radius R in the unit cube (n^3 cells) distorted by
//! f = 1 + distortion |x - c - a|^2 / 0.5 (distortion 0: the exact distance;
//! distortion < 0: Russo and Smereka's f = 0.1 + |x - c - a|^2), reinitialised
//! `iterations` times.  Drift: on every cell edge where phi changes sign, the
//! distance between its linear crossing and the exact crossing of the sphere
//! on that edge, / h (mean and max); and the max deviation of |grad phi|
//! from one within 3 cells of the sphere.
struct ReinitDrift { double meanDrift = 0, maxDrift = 0, gradError = 0; };
ReinitDrift MeasureReinitDrift(int n, ReinitScheme scheme, int iterations, double distortion, Pool* pool);

// ------------------------------------------------------------------ flow cases

//! Taylor-Green vortex, a steady solution of the Euler equations in the
//! closed box [0, pi]^2 (free-slip walls): u = sin x cos y, v = -cos x sin y,
//! p = (cos 2x + cos 2y) / 4.  n x n x 1 cells, full of liquid, no gravity;
//! run to `duration`.  Isolates momentum advection plus projection.
struct TaylorGreenResult { double maxError = 0, l2Error = 0, energyRatio = 0, seconds = 0; int steps = 0; };
TaylorGreenResult RunTaylorGreen(int n, double duration, const SolverOptions& options, Pool* pool);

//! Hydrostatic rest: a tank [0,1] x [0,1] (x [0,1] in 3-D) filled to 1/2
//! across the gravity direction, gravity tilted by `tilt` (radians, about z)
//! so the free surface is a tilted plane relative to the grid, optionally a
//! static solid sphere (radius 0.15 at (0.5, sphereY)) immersed; run for
//! `duration`.  Reports max |u| over the run / sqrt(g h) and the max pressure
//! error / (rho g H), H = 1/2, over cells wholly in the liquid.
struct HydrostaticResult { double maxVelocity = 0, pressureError = 0, volumeDrift = 0, seconds = 0; int steps = 0, iterations = 0; };
HydrostaticResult RunHydrostatic(int n, bool threeD, double tilt, bool sphere, double sphereY, double duration,
	const SolverOptions& options, Pool* pool);

//! Small-amplitude standing wave in a two-dimensional tank of length 1 m,
//! mode 2 (k = 2 pi), depth d = kd / k, eta = a cos(kx), a = ka / k, at rest:
//! reference omega^2 = g k tanh(k d).  `cells` across the tank (= per
//! wavelength).  The mode amplitude A(t) (projection of the column interface
//! heights on cos kx) gives the frequency (zero crossings over `periods`)
//! and the decay rate gamma (least squares of log |A| at its extrema).
struct WaveResult {
	double omega = 0, omegaExact = 0, frequencyError = 0;   // (omega - exact) / exact
	double decay = 0;          // gamma / omega_exact (per radian)
	double amplitudeError = 0; // max |A(t) - a cos(omega_exact t)| / a over the run
	double volumeShift = 0;    // sum of |volume-correction shifts| / a
	double seconds = 0;
	int steps = 0, crossings = 0;
};
WaveResult RunStandingWave(int cells, double kd, double ka, double periods, const SolverOptions& options, Pool* pool);

//! Sloshing in a rectangular tank a x b (x and z; a = 1, b = 0.6), depth
//! d = 0.5, eta = A1 cos(pi x / a) + A2 cos(pi z / b) at rest: the first two
//! natural frequencies against omega_mn^2 = g k_mn tanh(k_mn d), k_10 = pi/a,
//! k_01 = pi/b (linear theory).  `cells` across a.
struct SloshResult { double error10 = 0, error01 = 0, omega10 = 0, omega01 = 0, seconds = 0; int steps = 0; };
SloshResult RunSloshing(int cells, double periods, const SolverOptions& options, Pool* pool);

//! Early-time collapse of a two-dimensional column (height H = 1, length L,
//! against the wall x = 0, dam face x = L): the acceleration field at the
//! instant the dam is removed, from the potential-flow series derived in
//! docs/SIMULATION_M7A_RESULTS.md, against u / dt after one step from rest.
//! Interfaces off the grid (h = 1 / (cells + 0.3)).  Errors / g: the L2 norm
//! over the liquid's faces and the max over faces at least H/4 from the
//! bottom corner of the dam face (the series' log singularity).
struct CollapseResult { double l2 = 0, maxFar = 0, faceL2 = 0, seconds = 0; };
CollapseResult RunColumnCollapse(int cells, double length, const SolverOptions& options, Pool* pool);
//! The reference: (a_x, a_y) / g at (x, y) inside the column, N terms.
void CollapseAcceleration(double x, double y, double length, int terms, double& ax, double& ay);

//! The early-time collapse of case 5 against cut-cell solids (case 5s): the
//! column 0 <= x' <= L, 0 <= y' <= H (L = H = 1) of a frame rotated by `angle`
//! about the pivot P, its wall x' = 0 and bed y' = 0 two static solid planes
//! the grid cuts, gravity along -y'.  The reference is case 5's series in the
//! rotated frame (both solid walls are planes of symmetry of its even
//! extension).  h = 1 / cells in a 2.4 x 1.7 box.  The level set is the
//! distance to {x' <= L, y' <= H} (the liquid continues into the solids).
//! Errors / g of u / dt after one step from rest on every face with an open
//! part between two liquid cells, at the face centre: the max at least H/4
//! from the dam face's bottom corner (over all such faces, over those
//! within 2h of a solid, and over the rest) and the L2 norm; and the max
//! pressure error / (rho g H) at the centres of liquid cells inside the
//! column, H/4 from that corner.
struct SolidCollapseResult {
	double maxFar = 0, maxNearSolid = 0, maxInterior = 0, l2 = 0, pressure = 0, seconds = 0;
	std::size_t faces = 0, nearFaces = 0;
	double minFraction = 1;   // the smallest open fraction among the measured faces
};
SolidCollapseResult RunSolidCollapse(int cells, double angle, const V3& pivot, const SolverOptions& options, Pool* pool);

//! Dam break: a square column a = 1 (n^2 = 1 in Martin and Moyce's notation)
//! against the wall of a tank 6a long and 1.25a high; the surge front Z = z/a
//! (from the wall) against T = t sqrt(g/a).  `frontTimes` receives T at each
//! of Martin and Moyce's Z values the front passes (-1 if not reached).
struct DamBreakResult { std::vector<double> z, t; double seconds = 0; int steps = 0; double volumeDrift = 0; };
DamBreakResult RunDamBreak(int cells, double duration, const SolverOptions& options, Pool* pool);
//! Martin and Moyce 1952, table 1 (n^2 = 1, a = 2.25 in, mean of ~8 runs):
//! (Z, T) pairs, T normalised by the authors to T = 0.80 at Z = 1.44.
extern const double MartinMoyceSquare[][2];
extern const int MartinMoyceSquareCount;
//! Lobovsky et al. 2014 (J. Fluids Struct. 48, 407-434; arXiv 1308.0115): average downstream front speed
//! v / sqrt(g H) for t sqrt(g / H) > 1 (their table 1).  Their front positions are published as a figure
//! only, so no position table is carried (see docs/SIMULATION_M7_REFERENCES.md).
extern const double LobovskyFrontSpeed300, LobovskyFrontSpeed600, LobovskyReductionMartinMoyce57;
//! Their wall impact pressure peaks over 100 runs (tables 2 and 4): per sensor {height above the bed [mm],
//! median, 2.5 % percentile, 97.5 % percentile} in mbar; sensors 1 to 4 on the centre line.  Tank 1610 x 600
//! x 150 mm, the reservoir 600 mm long, the impact wall 1010 mm from the gate; H = 300 mm (reservoir 2H wide)
//! and H = 600 mm (square column, our geometry).  Pressures scale with rho g H (H = 0.3 m: 2943 Pa).
extern const double LobovskyImpact300[4][4];
extern const double LobovskyImpact600[4][4];
//! The 2-D square column of `RunDamBreak` in Lobovsky's tank (the impact wall 1010/600 of a column width
//! from the gate, 2 a high): peak P* = p / (rho g a) over the run at the wall cell nearest each of the four
//! sensor heights (3, 15, 30 and 80 mm of H = 600 mm), and the time T = t sqrt(g / a) of the sensor-1 peak.
struct DamBreakImpactResult { double peak[4] = {0, 0, 0, 0}; double peakTime = 0, seconds = 0; int steps = 0; bool finite = true; };
DamBreakImpactResult RunDamBreakImpact(int cells, double duration, const SolverOptions& options, Pool* pool);

//! Long sloshing run for volume conservation: the 2-D tank of the standing
//! wave, finite depth, larger amplitude (ka = 0.1), `periods` periods;
//! the relative liquid volume change per period (correction off) and the
//! correction's mean and max shift / h (correction on).
struct VolumeResult { double drift = 0, driftPerPeriod = 0, meanShift = 0, maxShift = 0, seconds = 0; };
VolumeResult RunVolumeConservation(int cells, double periods, bool correction, const SolverOptions& options, Pool* pool);

// ------------------------------------------------------------------ rigid bodies (m7b: case 9 and added mass)

//! A body for the cases below: mass m, the moments of its shape, at x, at
//! rest, gravity g as its external force (Dynamic).
LiquidBody MakeBody(const BodyShape& shape, BodyMotion motion, double mass, const V3& x, const V3& gravity);

//! Added mass of an impulsive start in potential flow.  A body at the centre
//! of a concentric container of radius b (a static cut-cell solid: the
//! complement of a circle or sphere) full of liquid at rest is set moving at
//! V = 1 m/s along x; the single (kinematic) projection gives the body the
//! impulse -m_a V.  2-D: a circular cylinder of radius a along z in a slab one
//! cell thick, m_a = rho pi a^2 (b^2 + a^2) / (b^2 - a^2) per unit length; 3-D:
//! a sphere, m_a = (rho V / 2) (b^3 + 2 a^3) / (b^3 - a^3) (both derived in
//! docs/SIMULATION_M7B_RESULTS.md section 4).  The box is [0, 1]^d, the centre
//! offset from the grid by `shift` cells along each axis; `cells` per unit.
struct AddedMassResult { double added = 0, exact = 0, error = 0, seconds = 0; int iterations = 0, steps = 0; double sideForce = 0; };
//! m7d: `shape` (3-D only; e.g. a tessellated sphere as a mesh body) replaces
//! the analytic sphere; the reference stays the sphere's with the shape's volume.
AddedMassResult RunAddedMassImpulse(int cells, bool threeD, double a, double b, double shift, const SolverOptions& options, Pool* pool,
	const BodyShape* shape = nullptr);
//! The same container in 2-D with the cylinder in prescribed oscillation
//! x = x0 + X sin(w t): the force's Fourier sine coefficient over `periods`
//! periods against m_a X w^2 (the nonlinear terms are O((X / b)^2) and
//! cancel by symmetry at the centre to first order); `error` relative.
AddedMassResult RunAddedMassOscillation(int cells, double a, double b, double amplitude, double frequency, double periods,
	const SolverOptions& options, Pool* pool);

//! Bobbing reference: the lowest symmetric natural frequency of linear
//! potential flow for a 2-D box of width w and draft d floating (density
//! ratio = draft / height) in a tank of width L with still depth D, the box
//! spanning the tank but for two gaps of (L - w) / 2.  Per frequency, Laplace
//! with Neumann walls and box sides, unit heave velocity on the box bottom,
//! and phi_y = (omega^2 / g) phi on the gap surfaces, solved by MINRES on a
//! cell-centred 5-point grid of `cells` across the half tank (the box edges
//! on grid lines; symmetric about the centre); omega is the root of
//! omega^2 (m + rho A(omega)) = rho g w, A = the integral of phi over the box
//! bottom (secant).  An independent check of the solver (a different
//! discretisation of the linear problem), not a closed form; its own
//! convergence is reported.
double BoxHeaveFrequency(double L, double w, double D, double d, int cells);
//! The long-wave (manometer) estimate of the same frequency: plug flow in the
//! gaps and lubrication-like flow under the box (an order-of-magnitude model).
double BoxHeaveManometer(double L, double w, double D, double d);
//! Bobbing in the solver: the box (height 2 d, density ratio 0.5, heave only)
//! released `offset` m below equilibrium at rest in a tank [0, L] x [0, 1]
//! filled to D, 2-D, `cells` across L; the frequency from the upward
//! crossings of the mean over `periods` periods of the reference.
struct BobResult { double omega = 0, decay = 0, seconds = 0; int steps = 0, crossings = 0; };
BobResult RunBoxBobbing(int cells, double L, double w, double D, double d, double offset, double periods,
	const SolverOptions& options, Pool* pool);

//! A heavy body released from rest at the centre of the concentric container
//! of RunAddedMassImpulse, full of liquid, under gravity (coupled in the
//! pressure solve): the initial acceleration (the first step's velocity / dt)
//! against (m - rho V) g / (m + m_a) with the container's exact m_a, and its
//! descent over `duration` against the constant-added-mass ODE (valid while
//! the offset stays small against b - a).
struct SinkResult { double acceleration = 0, exact = 0, error = 0, drop = 0, dropModel = 0, seconds = 0; int steps = 0, maxIterations = 0; };
SinkResult RunHeavySinking(int cells, bool threeD, double ratio, double duration, const SolverOptions& options, Pool* pool);

//! Archimedes: the submerged depth d of a floating sphere (3-D) or of a
//! horizontal circular cylinder (2-D) of radius R at density ratio `ratio`
//! (cap volume pi d^2 (3R - d) / 3 = ratio 4/3 pi R^3; segment area
//! R^2 acos(1 - d/R) - (R - d) sqrt(2 R d - d^2) = ratio pi R^2).
double FloatingDepth(bool threeD, double R, double ratio);

//! Floating statics: a sphere (3-D) or cylinder (2-D, axis z) of radius R =
//! 0.15 in a tank [0, 1]^d filled to y = eta = 0.5 at rest, the body held at
//! height y (kinematic, at rest): the net vertical force F(y) = buoyancy -
//! m g after one projection; the discrete equilibrium y* (F = 0, secant);
//! offset = (y* - y_exact) / R, y_exact = eta + R - d; imbalance = F(y_exact)
//! / (m g).  The body's centre is offset from the grid by `shift` cells in x
//! and z (0: on a node, symmetric).
struct FloatStatics { double offset = 0, imbalance = 0, yExact = 0, yDiscrete = 0, seconds = 0; int solves = 0; };
//! m7d: `shape` (3-D only) replaces the analytic sphere, with the mass ratio rho
//! times the shape's volume; the reference stays the sphere's.
FloatStatics RunFloatingStatics(int cells, bool threeD, double ratio, double shift, const SolverOptions& options, Pool* pool,
	const BodyShape* shape = nullptr);

//! Floating dynamics: the same tank with the body Dynamic (coupled in the
//! pressure solve), released at rest `start` R above (+) or below its
//! exact equilibrium, rotation and horizontal motion locked unless `free`;
//! run `duration` s.  Reported: the mean height over the run against the
//! equilibrium (the exact one, the liquid level corrected for the volume
//! the release displaced), the largest excursion / the initial one (> 1
//! would mean growth: the added-mass instability of a staggered exchange),
//! the dominant period (zero crossings of y - mean), the solver's largest
//! PCG iteration count, and the momentum audit of the run: the body's
//! momentum change minus gravity's impulse against the liquid's impulse
//! on it (its own sum; round-off), and the liquid + body vertical momentum
//! change minus the impulses of gravity and of the walls, / the impulse of
//! gravity on liquid and body (and / that on the body).
struct FloatDynamics {
	double meanOffset = 0;     // (mean y - y_eq) / R
	double growth = 0;         // max |y - ybar| / |y(0) - ybar|, ybar the mean over whole periods
	double lateGrowth = 0;     // the same over the second half of the run
	double period = 0;         // s
	double maxSpeed = 0;       // max body speed (m/s)
	double bodyAudit = 0;      // |m dv - m g T - sum impulses| / (m g T)
	double totalAudit = 0;     // |dP_total - G - W| / ((M + m) g T), M the liquid's mass
	double totalAuditBody = 0; // the same / (m g T)
	double yEq = 0, seconds = 0;
	int steps = 0, maxIterations = 0;
	bool finite = true;
	std::vector<double> t, y;  // the trajectory
};
FloatDynamics RunFloatingDynamics(int cells, bool threeD, double ratio, double start, double duration, bool free,
	const SolverOptions& options, Pool* pool);

//! The neutral body at rest: a sphere (3-D) or cylinder (2-D) with the
//! discrete neutral mass rho V_y (V_y the volume its closed y-faces give, the
//! volume the pressure's buoyancy sees) fully submerged in still liquid,
//! free in all degrees of freedom, centred on a grid node; run `duration`:
//! the largest body speed and liquid speed / sqrt(g h) (zero to the solver's
//! tolerance), and the same with the exact mass rho V: the body's mean
//! acceleration / g (the discrete buoyancy error).
struct NeutralResult { double bodySpeed = 0, liquidSpeed = 0, exactMassAcceleration = 0, volumeError = 0, seconds = 0; int steps = 0; };
NeutralResult RunNeutralBody(int cells, bool threeD, double duration, const SolverOptions& options, Pool* pool);

// ------------------------------------------------------------------ viscosity and surface tension (m7c: cases 10 and 11)

//! Linear waves on deep liquid with viscosity and surface tension (Lamb
//! 1932, section 349; derived in docs/SIMULATION_M7C_RESULTS.md): the root
//! s of (s + 2 nu k^2)^2 + g k + (sigma / rho) k^3 = 4 nu^2 k^3 m with
//! m = sqrt(k^2 + s / nu), Re m > 0, near -2 nu k^2 + i omega_0 (complex
//! Newton): the amplitude decay rate -Re s and the frequency Im s.
void ViscousWaveRoot(double g, double k, double nu, double sigmaOverRho, double& decay, double& omega);

//! The standing wave of RunStandingWave (tank 1 m, mode k = 2 pi) with a
//! kinematic viscosity nu (no-slip or free-slip walls and bottom): the
//! measured amplitude decay rate (1/s) against the deep-water root above
//! (`decayExact`; `decayLeading` = 2 nu k^2) and the inviscid run's own
//! decay (`inviscidDecay`, the numerical damping at that level).
struct ViscousWaveResult {
	double decay = 0, decayExact = 0, decayLeading = 0, inviscidDecay = 0, error = 0, errorNet = 0;   // error: decay / exact - 1; net: (decay - inviscid) / exact - 1
	double omega = 0, omegaExact = 0, seconds = 0;
	int steps = 0, iterations = 0;
};
ViscousWaveResult RunViscousWave(int cells, double kd, double ka, double periods, double nu, bool noSlip, bool inviscidToo,
	const SolverOptions& options, Pool* pool);

//! The Taylor-Green vortex of RunTaylorGreen with kinematic viscosity nu and
//! free-slip walls: an exact Navier-Stokes solution decaying as
//! exp(-2 nu t) (k^2 = 2; the shear strain vanishes, so free slip holds).
//! amplitudeError = A(T) / exp(-2 nu T) - 1 with A the projection of the
//! face velocities on the initial field; maxError the largest face error /
//! the exact amplitude.
struct ViscousTaylorGreenResult { double amplitudeError = 0, maxError = 0, seconds = 0; int steps = 0, iterations = 0; };
ViscousTaylorGreenResult RunViscousTaylorGreen(int n, double nu, double duration, const SolverOptions& options, Pool* pool);

//! Circular Couette flow (2-D, no slip): a prescribed cylinder of radius R1
//! spinning at omega inside a static cut-cell container of radius R2 (the
//! unit box, centre offset by `shift` cells), full of liquid, no gravity,
//! run `duration` s from rest.  Steady state u_theta = A r + B / r,
//! A = -omega R1^2 / (R2^2 - R1^2), B = omega R1^2 R2^2 / (R2^2 - R1^2), and
//! the torque on the cylinder per unit length -4 pi mu omega R1^2 R2^2 /
//! (R2^2 - R1^2).  profileError: the largest |u - u_exact| / (omega R1) over
//! the faces at least one cell from both walls; torqueError: the measured
//! viscous torque of the last step / exact - 1.  `startupTime` > 0 also
//! compares the profile at that time with a fine 1-D radial reference
//! (Crank-Nicolson, 4000 radial cells: startupError, same norm).
struct CouetteResult { double profileError = 0, torque = 0, pressureTorque = 0, torqueExact = 0, torqueError = 0, startupError = 0, seconds = 0; int steps = 0, maxIterations = 0; };
CouetteResult RunCouette(int cells, double R1, double R2, double omega, double nu, double duration, double shift, double startupTime,
	const SolverOptions& options, Pool* pool);
//! The 1-D reference of RunCouette's start-up: u_theta(r, t) from rest, the
//! inner wall at omega R1 from t = 0, the outer at rest.
double CouetteStartup(double R1, double R2, double omega, double nu, double t, double r);

//! Stokes drag on a translating cylinder in a concentric container (2-D, no
//! slip, no gravity): a prescribed cylinder of radius a moving at U along x
//! inside a static cut-cell container of radius b (the unit box), from rest
//! for `duration`.  Exact (Happel and Brenner; checked in the results doc by
//! the dissipation integral of the biharmonic solution): F = 4 pi mu U /
//! (ln(b/a) - (b^2 - a^2)/(b^2 + a^2)) per unit length.  error: the last
//! step's force (pressure and viscous) / exact - 1; viscousShare its viscous
//! part / the total.  The cylinder drifts U duration (keep it small against b - a).
struct DragResult { double force = 0, exact = 0, error = 0, viscousShare = 0, seconds = 0; int steps = 0, maxIterations = 0; };
DragResult RunCylinderDrag(int cells, double a, double b, double U, double nu, double duration, double shift, const SolverOptions& options, Pool* pool);

//! A disk (2-D, radius 0.3, unit box, no gravity) in rigid rotation at
//! omega: one viscous step must leave it unchanged (its strain vanishes;
//! a Laplacian with Neumann conditions would dissipate it at the free
//! surface).  stepChange: max |u - u_rigid| / (omega R) after one implicit
//! viscous step of the solver's operator alone; momentLoss: the relative
//! change of the angular momentum over `duration` with viscosity nu minus
//! the same without viscosity.
struct RotationResult { double stepChange = 0, momentLoss = 0, inviscidLoss = 0, seconds = 0; int steps = 0; };
RotationResult RunRotatingDisk(int cells, double nu, double duration, const SolverOptions& options, Pool* pool);

//! A sphere of radius a and density ratio `ratio` settling under gravity
//! from rest on the axis of a vertical cylindrical container (radius R, a
//! static cut-cell solid; height H) full of liquid of kinematic viscosity
//! nu, no slip, the body dynamic (coupled in the pressure and the viscous solves).
//! The terminal velocity (the mean over the last fifth of the run) against
//! Stokes' 2 (ratio - 1) g a^2 / (9 nu) times the wall factor of Haberman
//! and Sayre (1958) for lambda = a / R (`corrected`; Bohlin's 1960 series
//! as a cross-check, `bohlin`).  `cells` across the container's diameter.
struct SettlingResult {
	double velocity = 0, stokes = 0, corrected = 0, bohlin = 0, error = 0, reynolds = 0, drift = 0, seconds = 0;
	int steps = 0, maxIterations = 0;
	bool finite = true;
	std::vector<double> t, v;
};
SettlingResult RunSettlingSphere(int cells, double a, double R, double H, double ratio, double nu, double duration,
	const SolverOptions& options, Pool* pool);
//! Haberman and Sayre (1958) wall factor U / U_Stokes for a sphere on the
//! axis of a cylinder, lambda = a / R; Bohlin (1960) series of the same.
double HabermanSayre(double lambda);
double BohlinFactor(double lambda);

//! A static drop (zero gravity) of radius R = 0.25 at the centre of the
//! unit box (2-D: a cylinder, Laplace jump sigma / R; 3-D: a sphere,
//! 2 sigma / R), rho = 1, sigma = 1, dynamic viscosity mu (0: inviscid),
//! run `duration` capillary times t_s = sqrt(rho R^3 / sigma).  Reported:
//! the mean pressure over cells more than 2h inside / the exact jump - 1;
//! the largest face speed over the run as U / sqrt(sigma / (rho R)) and, if
//! viscous, the capillary number mu U / sigma; the speed at the end; the
//! largest curvature error at the interface cells at t = 0 (|kappa - 1/R|
//! R, 2-D; 2/R in 3-D).
struct DropResult {
	double pressureError = 0, maxSpeed = 0, finalSpeed = 0, capillary = 0, capillaryNumber = 0, curvatureError = 0, seconds = 0;
	int steps = 0;
};
DropResult RunStaticDrop(int cells, bool threeD, double duration, double mu, const SolverOptions& options, Pool* pool);

//! The oscillating drop: r = R (1 + epsilon cos(2 theta)) (2-D, the n = 2
//! mode of a liquid cylinder) or P2(cos theta) (3-D) in zero gravity,
//! R = 0.25, rho = sigma = 1, inviscid: the frequency of the second moment
//! (zero crossings over `periods`) against Lamb's omega^2 = n (n^2 - 1)
//! sigma / (rho R^3) (2-D) and n (n - 1) (n + 2) sigma / (rho R^3) (3-D),
//! n = 2; decay: its amplitude decay rate / omega (numerical, inviscid).
//! With a dynamic viscosity mu (nu = mu: rho = 1) the amplitude decays at the
//! irrotational rate gamma = 2 n (n - 1) nu / R^2 (2-D) or (n - 1)(2 n + 1) nu
//! / R^2 (3-D) (`decayExact`, 1/s; derived in the results doc), and the
//! frequency is sqrt(omega^2 - gamma^2); `decay` is then the measured rate
//! (1/s), else the measured rate / omega.
struct OscillationResult { double omega = 0, omegaExact = 0, error = 0, decay = 0, decayExact = 0, decayError = 0, seconds = 0; int steps = 0, crossings = 0; };
OscillationResult RunOscillatingDrop(int cells, bool threeD, double epsilon, double periods, double mu, const SolverOptions& options, Pool* pool);

//! The standing wave of RunStandingWave with surface tension sigma / rho
//! (m^3/s^2): reference omega^2 = (g k + (sigma / rho) k^3) tanh(k d).
WaveResult RunCapillaryWave(int cells, double kd, double ka, double sigmaOverRho, double periods, const SolverOptions& options, Pool* pool);

} } }

#endif
