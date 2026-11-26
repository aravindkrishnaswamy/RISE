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

//! Long sloshing run for volume conservation: the 2-D tank of the standing
//! wave, finite depth, larger amplitude (ka = 0.1), `periods` periods;
//! the relative liquid volume change per period (correction off) and the
//! correction's mean and max shift / h (correction on).
struct VolumeResult { double drift = 0, driftPerPeriod = 0, meanShift = 0, maxShift = 0, seconds = 0; };
VolumeResult RunVolumeConservation(int cells, double periods, bool correction, const SolverOptions& options, Pool* pool);

} } }

#endif
