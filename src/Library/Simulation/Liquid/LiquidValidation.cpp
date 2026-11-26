//////////////////////////////////////////////////////////////////////
//
//  LiquidValidation.cpp - see LiquidValidation.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidValidation.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
constexpr double Pi = 3.14159265358979323846;
inline double Sq(double x) { return x*x; }

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ transport

Grid UnitSquare(int n)
{
	Grid g;
	g.n = {{n, n, 1}};
	g.h = 1.0/n;
	g.origin = {{0, 0, -0.5*g.h}};
	return g;
}

// Exact characteristic functions (inside = true).
bool InZalesak(double x, double y)
{
	if (Sq(x - 0.5) + Sq(y - 0.75) > Sq(0.15)) return false;
	return !(std::fabs(x - 0.5) <= 0.025 && y <= 0.85);
}
bool InCircle(double x, double y) { return Sq(x - 0.5) + Sq(y - 0.75) <= Sq(0.15); }

// Signed distance-like initial level sets (reinitialised before the run).
double ZalesakPhi(double x, double y)
{
	const double disk = std::sqrt(Sq(x - 0.5) + Sq(y - 0.75)) - 0.15;
	Shape slot;
	slot.type = Shape::Type::Box;
	slot.lo = {{0.475, 0.0, -1}};
	slot.hi = {{0.525, 0.85, 1}};
	return std::max(disk, -slot.Distance({{x, y, 0}}));
}

// Areas of {phi < 0} (bilinear phi) and of its symmetric difference with
// the exact shape, by 32 x 32 subsamples in cells near either interface.
void Areas(const Grid& g, const std::vector<double>& phi, bool (*inside)(double, double), double& area, double& difference)
{
	const int s = 32;
	area = difference = 0;
	const double cell = g.h*g.h, sub = cell/(s*s);
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const double f = phi[g.C(i, j, 0)];
		const double x0 = g.origin[0] + i*g.h, y0 = g.origin[1] + j*g.h;
		// Cells far from both interfaces count whole.
		const bool exactIn = inside(x0 + 0.5*g.h, y0 + 0.5*g.h);
		bool uniform = std::fabs(f) > 2*g.h;
		if (uniform) {
			for (int c = 0; c < 4 && uniform; ++c)
				if (inside(x0 + (c & 1)*g.h, y0 + (c >> 1)*g.h) != exactIn) uniform = false;
			// The slot is 10 cells wide at the coarsest level tested; corners suffice there.
		}
		if (uniform) {
			const bool numIn = f < 0;
			if (numIn) area += cell;
			if (numIn != exactIn) difference += cell;
			continue;
		}
		for (int b = 0; b < s; ++b) for (int a = 0; a < s; ++a) {
			const double x = x0 + (a + 0.5)*g.h/s, y = y0 + (b + 0.5)*g.h/s;
			const bool numIn = SampleCells(g, phi, {{x, y, 0}}) < 0;
			if (numIn) area += sub;
			if (numIn != inside(x, y)) difference += sub;
		}
	}
}

// Face velocities of the rotation (exactly divergence-free: u depends on y, v on x).
void RotationFaces(const Grid& g, MacField& u)
{
	u.Resize(g);
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i <= g.n[0]; ++i) {
		const V3 x = g.FaceCentre(0, i, j, 0);
		u.u[0][g.F(0, i, j, 0)] = 2*Pi*(0.5 - x[1]);
	}
	for (int j = 0; j <= g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const V3 x = g.FaceCentre(1, i, j, 0);
		u.u[1][g.F(1, i, j, 0)] = 2*Pi*(x[0] - 0.5);
	}
}

// Face velocities of the vortex at time t: differences of psi at the corners.
void VortexFaces(const Grid& g, double t, double period, MacField& u)
{
	u.Resize(g);
	const double c = std::cos(Pi*t/period);
	auto psi = [&](int i, int j) {
		const double x = g.origin[0] + i*g.h, y = g.origin[1] + j*g.h;
		return c*Sq(std::sin(Pi*x))*Sq(std::sin(Pi*y))/Pi;
	};
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i <= g.n[0]; ++i)
		u.u[0][g.F(0, i, j, 0)] = -(psi(i, j + 1) - psi(i, j))/g.h;
	for (int j = 0; j <= g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i)
		u.u[1][g.F(1, i, j, 0)] = (psi(i + 1, j) - psi(i, j))/g.h;
}

TransportResult RunTransport(const Grid& g, std::vector<double> phi, bool (*inside)(double, double), double period,
	double umax, const std::function<void(double, MacField&)>& faces, const TransportOptions& o, Pool* pool)
{
	const double start = Now();
	TransportResult r;
	r.n = g.n[0];
	MacField u;
	std::array<std::vector<double>,3> cell;
	const CellVelocityAt velocity = [&](double t, std::array<std::vector<double>,3>& c) {
		faces(t, u);
		CellVelocity(g, u, c, pool);
	};
	const std::function<const MacField&(double)> mac = [&](double t) -> const MacField& { faces(t, u); return u; };
	MarkerParticles particles;
	if (o.particles) particles.Seed(g, phi, o.particle, 0, pool);
	double a0 = 0, d0 = 0;
	Areas(g, phi, inside, a0, d0);
	const double target = LiquidVolume(g, phi, nullptr, pool);
	const double dtmax = o.cfl*g.h/umax;
	const int steps = static_cast<int>(std::ceil(period/dtmax*(1 - 1e-12)));
	const double dt = period/steps;
	for (int s = 0; s < steps; ++s) {
		const double t = s*dt;
		if (o.particles) particles.Advect(g, mac, t, dt, pool);
		AdvectLevelSet(g, phi, velocity, t, dt, pool);
		if (o.particles) particles.Correct(g, phi, pool);
		if (o.reinit != ReinitScheme::None && (o.reinitThreshold <= 0 || GradientDeviation(g, phi, 3, pool) > o.reinitThreshold)) {
			Reinitialize(g, phi, o.reinit, o.reinitIterations, o.reinitCfl, pool);
			++r.reinitCalls;
		}
		if (o.particles) {
			particles.Correct(g, phi, pool);
			particles.AdjustRadii(g, phi, pool);
			if ((s + 1) % o.particle.reseedInterval == 0) particles.Reseed(g, phi, std::uint64_t(s + 1), pool);
		}
		if (o.volumeCorrection) {
			const double shift = CorrectVolume(g, phi, target, nullptr, pool);
			r.maxShift = std::max(r.maxShift, std::fabs(shift)/g.h);
			r.sumShift += std::fabs(shift)/g.h;
		}
	}
	double a1 = 0, d1 = 0;
	Areas(g, phi, inside, a1, d1);
	r.steps = steps;
	r.areaChange = (a1 - a0)/a0;
	r.areaError = a1;   // the caller divides by the exact area
	r.l1 = d1;
	r.particles = particles.Particles().size();
	r.seconds = Now() - start;
	(void)cell;
	return r;
}
} // namespace

TransportResult RunZalesak(int n, const TransportOptions& options, Pool* pool)
{
	const Grid g = UnitSquare(n);
	std::vector<double> phi(g.Cells());
	for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
		const V3 x = g.CellCentre(i, j, 0);
		phi[g.C(i, j, 0)] = ZalesakPhi(x[0], x[1]);
	}
	Reinitialize(g, phi, ReinitScheme::Subcell, 3*n/2, 0.3, pool);
	const double R = 0.15, w = 0.05;
	const double ys = 0.75 - std::sqrt(R*R - 0.25*w*w);
	const double area = Pi*R*R - (w*(0.85 - 0.75) + (0.5*w*std::sqrt(R*R - 0.25*w*w) + R*R*std::asin(0.5*w/R)));
	const double perimeter = R*(2*Pi - 2*std::asin(0.5*w/R)) + 2*(0.85 - ys) + w;
	const double umax = 2*Pi*std::sqrt(0.5);
	TransportResult r = RunTransport(g, phi, InZalesak, 1.0, umax, [&](double, MacField& u) { RotationFaces(g, u); }, options, pool);
	r.l1 /= perimeter;
	r.areaError = (r.areaError - area)/area;
	return r;
}

TransportResult RunVortex(int n, double period, const TransportOptions& options, Pool* pool)
{
	const Grid g = UnitSquare(n);
	std::vector<double> phi(g.Cells());
	for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
		const V3 x = g.CellCentre(i, j, 0);
		phi[g.C(i, j, 0)] = std::sqrt(Sq(x[0] - 0.5) + Sq(x[1] - 0.75)) - 0.15;
	}
	TransportResult r = RunTransport(g, phi, InCircle, period, 1.0,
		[&](double t, MacField& u) { VortexFaces(g, t, period, u); }, options, pool);
	r.l1 /= 2*Pi*0.15;
	r.areaError = (r.areaError - Pi*0.15*0.15)/(Pi*0.15*0.15);
	return r;
}

ReinitDrift MeasureReinitDrift(int n, ReinitScheme scheme, int iterations, double distortion, Pool* pool)
{
	Grid g;
	g.n = {{n, n, n}};
	g.h = 1.0/n;
	const V3 c{{0.5, 0.5, 0.5}};
	const double R = 0.3;
	std::vector<double> phi(g.Cells());
	for (int k = 0; k < n; ++k) for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
		const V3 x = g.CellCentre(i, j, k);
		const double r = std::sqrt(Sq(x[0] - c[0]) + Sq(x[1] - c[1]) + Sq(x[2] - c[2]));
		const double distort = distortion < 0 ? 0.1 + Sq(x[0] - c[0] - 0.3) + Sq(x[1] - c[1] - 0.2) + Sq(x[2] - c[2] - 0.1)
			: 1 + distortion*(Sq(x[0] - c[0] - 0.3) + Sq(x[1] - c[1] - 0.2) + Sq(x[2] - c[2] - 0.1))/0.5;
		phi[g.C(i, j, k)] = (r - R)*distort;
	}
	Reinitialize(g, phi, scheme, iterations, 0.3, pool);
	ReinitDrift d;
	double sum = 0;
	std::size_t count = 0;
	for (int k = 0; k < n; ++k) for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
		const double f = phi[g.C(i, j, k)];
		const int idx[3] = {i, j, k};
		for (int a = 0; a < 3; ++a) {
			if (idx[a] + 1 >= n) continue;
			int q[3] = {i, j, k};
			++q[a];
			const double f2 = phi[g.C(q[0], q[1], q[2])];
			if ((f < 0) == (f2 < 0)) continue;
			// The crossing of the numerical phi on this edge against the exact
			// crossing of the sphere on the same edge (the root of |x - c| = R).
			const V3 x0 = g.CellCentre(i, j, k);
			const double tn = f/(f - f2);
			double b = 0, cc = -R*R, aa = g.h*g.h;
			for (int m = 0; m < 3; ++m) { cc += Sq(x0[m] - c[m]); if (m == a) b = 2*g.h*(x0[m] - c[m]); }
			const double disc = std::max(0.0, b*b - 4*aa*cc);
			const double r1 = (-b - std::sqrt(disc))/(2*aa), r2 = (-b + std::sqrt(disc))/(2*aa);
			const double te = std::fabs(r1 - tn) < std::fabs(r2 - tn) ? r1 : r2;
			const double e = std::fabs(tn - te);
			sum += e;
			++count;
			d.maxDrift = std::max(d.maxDrift, e);
		}
	}
	d.meanDrift = count ? sum/double(count) : 0;
	for (int k = 1; k + 1 < n; ++k) for (int j = 1; j + 1 < n; ++j) for (int i = 1; i + 1 < n; ++i) {
		const V3 x = g.CellCentre(i, j, k);
		const double r = std::sqrt(Sq(x[0] - c[0]) + Sq(x[1] - c[1]) + Sq(x[2] - c[2]));
		if (std::fabs(r - R) > 3*g.h) continue;
		const double gx = (phi[g.C(i + 1, j, k)] - phi[g.C(i - 1, j, k)])/(2*g.h);
		const double gy = (phi[g.C(i, j + 1, k)] - phi[g.C(i, j - 1, k)])/(2*g.h);
		const double gz = (phi[g.C(i, j, k + 1)] - phi[g.C(i, j, k - 1)])/(2*g.h);
		d.gradError = std::max(d.gradError, std::fabs(std::sqrt(gx*gx + gy*gy + gz*gz) - 1));
	}
	return d;
}

} } }

// ======================================================================== flow cases

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
// Interface height of column (i, k): the first crossing from below (linear).
double ColumnHeight(const Grid& g, const std::vector<double>& phi, int i, int k)
{
	for (int j = 0; j + 1 < g.n[1]; ++j) {
		const double a = phi[g.C(i, j, k)], b = phi[g.C(i, j + 1, k)];
		if (a < 0 && b >= 0) return g.origin[1] + (j + 0.5)*g.h + g.h*a/(a - b);
	}
	return phi[g.C(i, 0, k)] >= 0 ? g.origin[1] : g.origin[1] + g.n[1]*g.h;
}

// Zero crossings of a sampled signal (linear in time) and the frequency they imply.
double CrossingFrequency(const std::vector<double>& t, const std::vector<double>& a, int& count)
{
	std::vector<double> z;
	for (std::size_t m = 1; m < a.size(); ++m)
		if ((a[m - 1] < 0) != (a[m] < 0)) z.push_back(t[m - 1] + (t[m] - t[m - 1])*a[m - 1]/(a[m - 1] - a[m]));
	count = int(z.size());
	if (z.size() < 2) return 0;
	return 3.14159265358979323846*double(z.size() - 1)/(z.back() - z.front());
}

// Decay rate of |a| from its extrema (parabolic refinement), least squares of log |a|.
double DecayRate(const std::vector<double>& t, const std::vector<double>& a)
{
	std::vector<double> tx, la;
	for (std::size_t m = 1; m + 1 < a.size(); ++m) {
		const double y0 = std::fabs(a[m - 1]), y1 = std::fabs(a[m]), y2 = std::fabs(a[m + 1]);
		if (!(y1 > y0 && y1 >= y2)) continue;
		const double den = y0 - 2*y1 + y2;
		const double off = den != 0 ? 0.5*(y0 - y2)/den : 0.0;
		const double peak = y1 - 0.25*(y0 - y2)*off;
		const double dt = t[m + 1] - t[m];
		tx.push_back(t[m] + off*dt);
		la.push_back(std::log(peak));
	}
	if (tx.size() < 2) return 0;
	double st = 0, sl = 0, stt = 0, stl = 0;
	const double n = double(tx.size());
	for (std::size_t m = 0; m < tx.size(); ++m) { st += tx[m]; sl += la[m]; stt += tx[m]*tx[m]; stl += tx[m]*la[m]; }
	return -(n*stl - st*sl)/(n*stt - st*st);
}

LiquidSetup Tank2D(int nx, int ny, double h)
{
	LiquidSetup s;
	s.grid.n = {{nx, ny, 1}};
	s.grid.h = h;
	s.grid.origin = {{0, 0, -0.5*h}};
	return s;
}
} // namespace

TaylorGreenResult RunTaylorGreen(int n, double duration, const SolverOptions& options, Pool* pool)
{
	(void)pool;
	const double start = Now();
	TaylorGreenResult r;
	LiquidSetup s = Tank2D(n, n, Pi/n);
	s.gravity = {{0, 0, 0}};
	Shape all;
	all.type = Shape::Type::Box;
	all.lo = {{-1, -1, -1}};
	all.hi = {{Pi + 1, Pi + 1, 1}};
	s.liquid.push_back(all);
	SolverOptions o = options;
	o.reinit = ReinitScheme::None;
	o.volumeCorrection = false;
	o.particles = false;
	LiquidModel m;
	std::string error;
	const unsigned threads = pool ? pool->Threads() : 1;
	if (!m.Initialize(s, o, threads, error)) { r.maxError = 1e300; return r; }
	const Grid& g = m.GridSpec();
	auto exact = [&](int a, int i, int j) {
		const V3 x = g.FaceCentre(a, i, j, 0);
		return a == 0 ? std::sin(x[0])*std::cos(x[1]) : -std::cos(x[0])*std::sin(x[1]);
	};
	for (int a = 0; a < 2; ++a) {
		const auto d = g.FaceDims(a);
		for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) m.u.u[a][g.F(a, i, j, 0)] = exact(a, i, j);
	}
	if (!m.ProjectState(error)) { r.maxError = 1e300; return r; }
	double e0 = 0;
	for (int a = 0; a < 2; ++a) for (double v : m.u.u[a]) e0 += v*v;
	if (!m.Run(duration, duration/20, [&](double, const StepStats&) { ++r.steps; }, error)) { r.maxError = 1e300; return r; }
	double sum = 0, e1 = 0;
	std::size_t count = 0;
	for (int a = 0; a < 2; ++a) {
		const auto d = g.FaceDims(a);
		for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
			const double v = m.u.u[a][g.F(a, i, j, 0)], e = v - exact(a, i, j);
			r.maxError = std::max(r.maxError, std::fabs(e));
			sum += e*e;
			e1 += v*v;
			++count;
		}
	}
	r.l2Error = std::sqrt(sum/double(count));
	r.energyRatio = e1/e0;
	r.seconds = Now() - start;
	return r;
}

HydrostaticResult RunHydrostatic(int n, bool threeD, double tilt, bool sphere, double sphereY, double duration,
	const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	HydrostaticResult r;
	LiquidSetup s;
	s.grid.n = {{n, n, threeD ? n : 1}};
	s.grid.h = 1.0/n;
	if (!threeD) s.grid.origin = {{0, 0, 0.5 - 0.5*s.grid.h}};
	const double gm = 9.81;
	s.gravity = {{gm*std::sin(tilt), -gm*std::cos(tilt), 0}};
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0.5, 0.5, 0.5}};
	fill.axis = {{-std::sin(tilt), std::cos(tilt), 0}};
	s.liquid.push_back(fill);
	if (sphere) {
		Shape sp;
		sp.type = Shape::Type::Sphere;
		sp.point = {{0.5, sphereY, 0.5}};
		sp.radius = 0.15;
		s.solids.push_back(sp);
	}
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) { r.maxVelocity = 1e300; return r; }
	if (!m.Run(duration, 1.0/30, [&](double, const StepStats& st) {
		++r.steps;
		r.iterations = std::max(r.iterations, st.pressure.iterations);
		r.maxVelocity = std::max(r.maxVelocity, m.MaxSpeed());
	}, error)) { r.maxVelocity = 1e300; return r; }
	r.maxVelocity /= std::sqrt(gm*s.grid.h);
	const Grid& g = m.GridSpec();
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const std::size_t c = g.C(i, j, k);
		if (!(m.phi[c] < 0) || (sphere && m.openCells[c] < 1)) continue;
		const V3 x = g.CellCentre(i, j, k);
		const double depth = -((x[0] - 0.5)*fill.axis[0] + (x[1] - 0.5)*fill.axis[1]);
		r.pressureError = std::max(r.pressureError, std::fabs(m.Pressure(c) - s.density*gm*depth));
	}
	r.pressureError /= s.density*gm*0.5;
	r.volumeDrift = m.Volume()/m.TargetVolume() - 1;
	r.seconds = Now() - start;
	return r;
}

WaveResult RunStandingWave(int cells, double kd, double ka, double periods, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	WaveResult r;
	const double L = 1, k = 2*Pi/L, d = kd/k, a = ka/k, gm = 9.81, h = L/cells;
	const int ny = static_cast<int>(std::ceil((d + std::max(0.125, 6*h))/h));
	LiquidSetup s = Tank2D(cells, ny, h);
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) { r.frequencyError = 1e300; return r; }
	const Grid& g = m.GridSpec();
	std::vector<double> phi(g.Cells());
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const V3 x = g.CellCentre(i, j, 0);
		phi[g.C(i, j, 0)] = x[1] - (d + a*std::cos(k*x[0]));
	}
	if (!m.SetLevelSet(phi, error)) { r.frequencyError = 1e300; return r; }
	r.omegaExact = std::sqrt(gm*k*std::tanh(k*d));
	auto amplitude = [&]() {
		double s2 = 0;
		for (int i = 0; i < g.n[0]; ++i) s2 += ColumnHeight(g, m.phi, i, 0)*std::cos(k*(i + 0.5)*h);
		return 2*s2/g.n[0];
	};
	std::vector<double> ts{0}, as{amplitude()};
	const double T = periods*2*Pi/r.omegaExact;
	if (!m.Run(T, T/200, [&](double t, const StepStats& st) {
		++r.steps;
		ts.push_back(t);
		as.push_back(amplitude());
		r.volumeShift += std::fabs(st.volumeShift)/a;
		r.amplitudeError = std::max(r.amplitudeError, std::fabs(as.back() - a*std::cos(r.omegaExact*t))/a);
	}, error)) { r.frequencyError = 1e300; return r; }
	r.omega = CrossingFrequency(ts, as, r.crossings);
	r.frequencyError = (r.omega - r.omegaExact)/r.omegaExact;
	r.decay = DecayRate(ts, as)/r.omegaExact;
	r.seconds = Now() - start;
	return r;
}

SloshResult RunSloshing(int cells, double periods, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	SloshResult r;
	const double A = 1, B = 0.6, d = 0.5, gm = 9.81, h = A/cells, amp = 0.005;
	LiquidSetup s;
	s.grid.n = {{cells, static_cast<int>(std::ceil((d + std::max(0.15, 6*h))/h)), static_cast<int>(std::lround(B/h))}};
	s.grid.h = h;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) { r.error10 = r.error01 = 1e300; return r; }
	const Grid& g = m.GridSpec();
	std::vector<double> phi(g.Cells());
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const V3 x = g.CellCentre(i, j, k);
		phi[g.C(i, j, k)] = x[1] - (d + amp*std::cos(Pi*x[0]/A) + amp*std::cos(Pi*x[2]/B));
	}
	if (!m.SetLevelSet(phi, error)) { r.error10 = r.error01 = 1e300; return r; }
	const double k10 = Pi/A, k01 = Pi/B;
	const double w10 = std::sqrt(gm*k10*std::tanh(k10*d)), w01 = std::sqrt(gm*k01*std::tanh(k01*d));
	auto amplitudes = [&](double& a10, double& a01) {
		a10 = a01 = 0;
		for (int k = 0; k < g.n[2]; ++k) for (int i = 0; i < g.n[0]; ++i) {
			const double eta = ColumnHeight(g, m.phi, i, k);
			a10 += eta*std::cos(Pi*(i + 0.5)*h/A);
			a01 += eta*std::cos(Pi*(k + 0.5)*h/B);
		}
		a10 *= 2.0/(double(g.n[0])*g.n[2]);
		a01 *= 2.0/(double(g.n[0])*g.n[2]);
	};
	std::vector<double> ts{0}, s10, s01;
	{ double x, y; amplitudes(x, y); s10.push_back(x); s01.push_back(y); }
	const double T = periods*2*Pi/w10;
	if (!m.Run(T, T/100, [&](double t, const StepStats&) {
		++r.steps;
		double x, y;
		amplitudes(x, y);
		ts.push_back(t); s10.push_back(x); s01.push_back(y);
	}, error)) { r.error10 = r.error01 = 1e300; return r; }
	int c1 = 0, c2 = 0;
	r.omega10 = CrossingFrequency(ts, s10, c1);
	r.omega01 = CrossingFrequency(ts, s01, c2);
	r.error10 = (r.omega10 - w10)/w10;
	r.error01 = (r.omega01 - w01)/w01;
	r.seconds = Now() - start;
	return r;
}

void CollapseAcceleration(double x, double y, double length, int terms, double& ax, double& ay)
{
	ax = ay = 0;
	for (int n = 0; n < terms; ++n) {
		const double l = (2*n + 1)*Pi/2;
		const double e2 = std::exp(-2*l*length);
		const double sh = (std::exp(l*(x - length)) - std::exp(-l*(x + length)))/(1 + e2);   // sinh(l x) / cosh(l L)
		const double ch = (std::exp(l*(x - length)) + std::exp(-l*(x + length)))/(1 + e2);   // cosh(l x) / cosh(l L)
		ax += 2*std::cos(l*y)*sh/l;
		ay -= 2*std::sin(l*y)*ch/l;
	}
}

CollapseResult RunColumnCollapse(int cells, double length, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	CollapseResult r;
	const double gm = 9.81, H = 1, h = 1.0/(cells + 0.3);
	LiquidSetup s = Tank2D(static_cast<int>(std::ceil((length + 0.6)/h)), static_cast<int>(std::ceil(1.3/h)), h);
	Shape col;
	col.type = Shape::Type::Box;
	col.lo = {{-1, -1, -1}};
	col.hi = {{length, H, 1}};
	s.liquid.push_back(col);
	SolverOptions o = options;
	o.particles = false;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, o, pool ? pool->Threads() : 1, error)) { r.l2 = 1e300; return r; }
	const double dt = 1e-5;
	StepStats st;
	if (!m.Step(dt, st, error)) { r.l2 = 1e300; return r; }
	const Grid& g = m.GridSpec();
	const int terms = 40*cells;
	double sum = 0, fsum = 0;
	std::size_t count = 0, fcount = 0;
	for (int a = 0; a < 2; ++a) {
		const auto d = g.FaceDims(a);
		for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
			const V3 x = g.FaceCentre(a, i, j, 0);
			if (!(x[0] > 0 && x[0] < length && x[1] > 0 && x[1] < H)) continue;
			// Faces whose two cells are liquid (the solve's own faces).
			const bool interior = a == 0 ? (i > 0 && i < g.n[0] && m.phi[g.C(i - 1, j, 0)] < 0 && m.phi[g.C(i, j, 0)] < 0)
				: (j > 0 && j < g.n[1] && m.phi[g.C(i, j - 1, 0)] < 0 && m.phi[g.C(i, j, 0)] < 0);
			if (!interior) continue;
			double ax, ay;
			CollapseAcceleration(x[0], x[1], length, terms, ax, ay);
			const double num = m.u.u[a][g.F(a, i, j, 0)]/dt/gm;
			const double e = num - (a == 0 ? ax : ay);
			sum += e*e;
			++count;
			const double dx = x[0] - length, dy = x[1];
			if (dx*dx + dy*dy >= 0.0625) r.maxFar = std::max(r.maxFar, std::fabs(e));
			if (a == 0 && x[0] > length - h) { fsum += e*e; ++fcount; }
		}
	}
	r.l2 = std::sqrt(sum/double(std::max<std::size_t>(1, count)));
	r.faceL2 = std::sqrt(fsum/double(std::max<std::size_t>(1, fcount)));
	r.seconds = Now() - start;
	return r;
}

SolidCollapseResult RunSolidCollapse(int cells, double angle, const V3& pivot, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	SolidCollapseResult r;
	const double gm = 9.81, H = 1, L = 1, h = 1.0/cells;
	const double ca = std::cos(angle), sa = std::sin(angle);
	const V3 e1{{ca, sa, 0}}, e2{{-sa, ca, 0}};
	LiquidSetup s = Tank2D(static_cast<int>(std::ceil(2.4/h)), static_cast<int>(std::ceil(1.7/h)), h);
	s.gravity = {{gm*sa, -gm*ca, 0}};
	Shape wall, bed;
	wall.type = bed.type = Shape::Type::Plane;
	wall.point = bed.point = V3{{pivot[0], pivot[1], 0}};
	wall.axis = e1;   // solid where x' < 0
	bed.axis = e2;    // solid where y' < 0
	s.solids = {wall, bed};
	SolverOptions o = options;
	o.particles = false;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, o, pool ? pool->Threads() : 1, error)) { r.l2 = 1e300; return r; }
	const Grid& g = m.GridSpec();
	auto local = [&](const V3& x, double& xl, double& yl) {
		const double dx = x[0] - pivot[0], dy = x[1] - pivot[1];
		xl = dx*e1[0] + dy*e1[1];
		yl = dx*e2[0] + dy*e2[1];
	};
	std::vector<double> phi(g.Cells());
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		double xl, yl;
		local(g.CellCentre(i, j, 0), xl, yl);
		const double dx = xl - L, dy = yl - H;
		phi[g.C(i, j, 0)] = dx <= 0 && dy <= 0 ? std::max(dx, dy) : std::sqrt(Sq(std::max(dx, 0.0)) + Sq(std::max(dy, 0.0)));
	}
	if (!m.SetLevelSet(phi, error)) { r.l2 = 1e300; return r; }
	const double dt = 1e-5;
	StepStats st;
	if (!m.Step(dt, st, error)) { r.l2 = 1e300; return r; }
	const int terms = 40*cells;
	double sum = 0;
	for (int a = 0; a < 2; ++a) {
		const auto d = g.FaceDims(a);
		for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
			const std::size_t f = g.F(a, i, j, 0);
			const int idx[2] = {i, j};
			if (!(m.fraction[a][f] > 0) || idx[a] == 0 || idx[a] == g.n[a]) continue;
			const std::size_t cl = a == 0 ? g.C(i - 1, j, 0) : g.C(i, j - 1, 0), cr = g.C(i, j, 0);
			if (!(m.phi[cl] < 0 && m.phi[cr] < 0)) continue;
			double xl, yl;
			local(g.FaceCentre(a, i, j, 0), xl, yl);
			if (xl > L || yl > H) continue;
			double ax, ay;
			CollapseAcceleration(xl, yl, L, terms, ax, ay);
			const double ref = ax*e1[a] + ay*e2[a];
			const double e = m.u.u[a][f]/dt/gm - ref;
			sum += e*e;
			++r.faces;
			r.minFraction = std::min(r.minFraction, m.fraction[a][f]);
			if (Sq(xl - L) + Sq(yl) < 0.0625) continue;
			r.maxFar = std::max(r.maxFar, std::fabs(e));
			if (xl < 2*h || yl < 2*h) { r.maxNearSolid = std::max(r.maxNearSolid, std::fabs(e)); ++r.nearFaces; }
			else r.maxInterior = std::max(r.maxInterior, std::fabs(e));
		}
	}
	r.l2 = std::sqrt(sum/double(std::max<std::size_t>(1, r.faces)));
	// p = rho g (H - y') + sum A_n cos(l y') cosh(l x') / cosh(l L), A_n = -2 rho g / (H l^2) (section 4).
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const std::size_t c = g.C(i, j, 0);
		if (!(m.phi[c] < 0)) continue;
		double xl, yl;
		local(g.CellCentre(i, j, 0), xl, yl);
		if (!(xl > 0 && yl > 0 && xl < L && yl < H) || Sq(xl - L) + Sq(yl) < 0.0625) continue;
		double pe = H - yl;
		for (int k = 0; k < terms; ++k) {
			const double l = (2*k + 1)*Pi/2;
			pe -= 2/(H*l*l)*std::cos(l*yl)*(std::exp(l*(xl - L)) + std::exp(-l*(xl + L)))/(1 + std::exp(-2*l*L));
		}
		r.pressure = std::max(r.pressure, std::fabs(m.Pressure(c)/(s.density*gm*H) - pe));
	}
	r.seconds = Now() - start;
	return r;
}

const double MartinMoyceSquare[][2] = {
	{1.11, 0.43}, {1.22, 0.62}, {1.44, 0.80}, {1.67, 0.97}, {1.89, 1.14}, {2.11, 1.29}, {2.33, 1.45}, {2.56, 1.62},
	{2.78, 1.76}, {3.00, 1.93}, {3.22, 2.07}, {3.44, 2.24}, {3.67, 2.40}, {3.89, 2.54}, {4.11, 2.71}, {4.33, 2.87},
	{4.56, 3.04}, {4.78, 3.21}, {4.89, 3.29}};
const int MartinMoyceSquareCount = int(sizeof(MartinMoyceSquare)/sizeof(MartinMoyceSquare[0]));

DamBreakResult RunDamBreak(int cells, double duration, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	DamBreakResult r;
	const double h = 1.0/cells;
	LiquidSetup s = Tank2D(6*cells, static_cast<int>(std::lround(1.25*cells)), h);
	Shape col;
	col.type = Shape::Type::Box;
	col.lo = {{-1, -1, -1}};
	col.hi = {{1, 1, 1}};
	s.liquid.push_back(col);
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) return r;
	const Grid& g = m.GridSpec();
	const double scale = std::sqrt(9.81);   // T = t sqrt(g / a), a = 1
	auto front = [&]() {
		for (int i = g.n[0] - 1; i >= 0; --i) {
			const double f = m.phi[g.C(i, 0, 0)];
			if (f < 0) {
				if (i + 1 < g.n[0]) { const double f2 = m.phi[g.C(i + 1, 0, 0)]; return (i + 0.5)*h + h*f/(f - f2); }
				return g.n[0]*h;
			}
		}
		return 0.0;
	};
	std::vector<double> ft{0}, fx{front()};
	if (!m.Run(duration, 1.0/120, [&](double t, const StepStats&) { ++r.steps; ft.push_back(t*scale); fx.push_back(front()); }, error))
		return r;
	for (int q = 0; q < MartinMoyceSquareCount; ++q) {
		const double Z = MartinMoyceSquare[q][0];
		double T = -1;   // not reached
		for (std::size_t s2 = 1; s2 < fx.size(); ++s2)
			if (fx[s2] >= Z && fx[s2 - 1] < Z) { T = ft[s2 - 1] + (ft[s2] - ft[s2 - 1])*(Z - fx[s2 - 1])/(fx[s2] - fx[s2 - 1]); break; }
		r.z.push_back(Z);
		r.t.push_back(T);
	}
	r.volumeDrift = m.Volume()/m.TargetVolume() - 1;
	r.seconds = Now() - start;
	return r;
}

VolumeResult RunVolumeConservation(int cells, double periods, bool correction, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	VolumeResult r;
	const double L = 1, k = 2*Pi/L, d = 1/k, a = 0.1/k, h = L/cells;
	const int ny = static_cast<int>(std::ceil((d + std::max(0.15, 6*h))/h));
	LiquidSetup s = Tank2D(cells, ny, h);
	SolverOptions o = options;
	o.volumeCorrection = correction;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, o, pool ? pool->Threads() : 1, error)) { r.drift = 1e300; return r; }
	const Grid& g = m.GridSpec();
	std::vector<double> phi(g.Cells());
	for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const V3 x = g.CellCentre(i, j, 0);
		phi[g.C(i, j, 0)] = x[1] - (d + a*std::cos(k*x[0]));
	}
	if (!m.SetLevelSet(phi, error)) { r.drift = 1e300; return r; }
	const double omega = std::sqrt(9.81*k*std::tanh(k*d));
	const double T = periods*2*Pi/omega;
	double sum = 0;
	std::size_t steps = 0;
	if (!m.Run(T, T/(20*periods), [&](double, const StepStats& st) {
		const double sh = std::fabs(st.volumeShift)/h;
		sum += sh;
		r.maxShift = std::max(r.maxShift, sh);
		++steps;
	}, error)) { r.drift = 1e300; return r; }
	r.meanShift = steps ? sum/double(steps) : 0;
	r.drift = m.Volume()/m.TargetVolume() - 1;
	r.driftPerPeriod = r.drift/periods;
	r.seconds = Now() - start;
	return r;
}

// ------------------------------------------------------------------ rigid bodies (m7b)

LiquidBody MakeBody(const BodyShape& shape, BodyMotion motion, double mass, const V3& x, const V3& gravity)
{
	LiquidBody b;
	b.name = "body";
	b.shape = shape;
	b.motion = motion;
	b.state.mass = mass;
	b.state.moments = shape.Moments(mass);
	b.state.x = x;
	b.x0 = x;
	for (int a = 0; a < 3; ++a) b.externalForce[std::size_t(a)] = mass*gravity[std::size_t(a)];
	return b;
}

namespace {
// The unit box [0, 1]^d: 3-D n^3, 2-D n x n x 1 with the slab centred on z = 0.
LiquidSetup UnitBox(int n, bool threeD)
{
	LiquidSetup s;
	s.grid.n = {{n, n, threeD ? n : 1}};
	s.grid.h = 1.0/n;
	s.grid.origin = {{0, 0, threeD ? 0.0 : -0.5/n}};
	return s;
}
// A sphere (3-D) or a cylinder along z through the slab (2-D), radius R.
BodyShape RoundBody(bool threeD, double R, double h)
{
	BodyShape b;
	b.type = threeD ? BodyShape::Type::Sphere : BodyShape::Type::Cylinder;
	b.radius = R;
	b.height = 4*h;   // beyond the slab: its caps never cut a face
	return b;
}
double RoundVolume(bool threeD, double R, double h) { return threeD ? 4.0/3.0*Pi*R*R*R : Pi*R*R*h; }
// Submerged volume of a round body of radius R whose lowest point is `depth` below the surface.
double Submerged(bool threeD, double R, double h, double depth)
{
	const double d = std::min(2*R, std::max(0.0, depth));
	if (threeD) return Pi*d*d*(3*R - d)/3;
	return h*(R*R*std::acos(1 - d/R) - (R - d)*std::sqrt(std::max(0.0, 2*R*d - d*d)));
}
} // namespace

AddedMassResult RunAddedMassImpulse(int cells, bool threeD, double a, double b, double shift, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	AddedMassResult r;
	LiquidSetup s = UnitBox(cells, threeD);
	const double h = s.grid.h, rho = s.density, V = 1;
	s.gravity = {{0, 0, 0}};
	const V3 c{{0.5 + shift*h, 0.5 + shift*h, threeD ? 0.5 + shift*h : 0.0}};
	Shape container;
	container.type = threeD ? Shape::Type::Sphere : Shape::Type::Cylinder;
	container.point = c;
	container.axis = {{0, 0, 1}};
	container.radius = b;
	container.height = 10;
	container.invert = true;
	s.solids.push_back(container);
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, 2, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	LiquidBody body = MakeBody(RoundBody(threeD, a, h), BodyMotion::Prescribed, 1, c, s.gravity);
	body.v0 = {{V, 0, 0}};
	s.bodies.push_back(body);
	SolverOptions o = options;
	o.particles = false;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, o, pool ? pool->Threads() : 1, error)) { r.error = 1e300; return r; }
	r.added = -m.bodyImpulse[0][0]/V;
	r.sideForce = std::fabs(m.bodyImpulse[0][1])/std::max(1e-300, std::fabs(m.bodyImpulse[0][0]));
	r.exact = threeD ? 0.5*rho*RoundVolume(true, a, h)*(b*b*b + 2*a*a*a)/(b*b*b - a*a*a) :
		rho*Pi*a*a*(b*b + a*a)/(b*b - a*a)*h;
	r.error = r.added/r.exact - 1;
	r.seconds = Now() - start;
	return r;
}

AddedMassResult RunAddedMassOscillation(int cells, double a, double b, double amplitude, double frequency, double periods,
	const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	AddedMassResult r;
	LiquidSetup s = UnitBox(cells, false);
	const double h = s.grid.h, rho = s.density, w = 2*Pi*frequency;
	s.gravity = {{0, 0, 0}};
	const V3 c{{0.5, 0.5, 0.0}};
	Shape container;
	container.type = Shape::Type::Cylinder;
	container.point = c;
	container.axis = {{0, 0, 1}};
	container.radius = b;
	container.height = 10;
	container.invert = true;
	s.solids.push_back(container);
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, 2, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	LiquidBody body = MakeBody(RoundBody(false, a, h), BodyMotion::Prescribed, 1, c, s.gravity);
	body.amplitude = {{amplitude, 0, 0}};
	body.frequency = frequency;
	s.bodies.push_back(body);
	SolverOptions o = options;
	o.particles = false;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, o, pool ? pool->Threads() : 1, error)) { r.error = 1e300; return r; }
	r.exact = rho*Pi*a*a*(b*b + a*a)/(b*b - a*a)*h;
	const double T = periods/frequency;
	double sine = 0, cosine = 0;
	if (!m.Run(T, 1/(40*frequency), [&](double t, const StepStats& st) {
		const double tm = t - 0.5*st.dt;
		const double F = m.bodyImpulse[0][0];   // the step's impulse: F dt
		sine += F*std::sin(w*tm);
		cosine += F*std::cos(w*tm);
		r.iterations = std::max(r.iterations, st.pressure.iterations);
		++r.steps;
	}, error)) { r.error = 1e300; return r; }
	sine *= 2/T;
	cosine *= 2/T;
	r.added = sine/(amplitude*w*w);
	r.sideForce = std::fabs(cosine)/std::max(1e-300, std::fabs(sine));
	r.error = r.added/r.exact - 1;
	r.seconds = Now() - start;
	return r;
}

SinkResult RunHeavySinking(int cells, bool threeD, double ratio, double duration, const SolverOptions& options, Pool* pool)
{
	const double begin = Now();
	SinkResult r;
	const double a = 0.15, b = 0.4;
	LiquidSetup s = UnitBox(cells, threeD);
	const double h = s.grid.h, rho = s.density, g = 9.81;
	const V3 c{{0.5, 0.5, threeD ? 0.5 : 0.0}};
	Shape container;
	container.type = threeD ? Shape::Type::Sphere : Shape::Type::Cylinder;
	container.point = c;
	container.axis = {{0, 0, 1}};
	container.radius = b;
	container.height = 10;
	container.invert = true;
	s.solids.push_back(container);
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, 2, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	const double V = RoundVolume(threeD, a, h), mass = ratio*rho*V;
	LiquidBody body = MakeBody(RoundBody(threeD, a, h), BodyMotion::Dynamic, mass, c, s.gravity);
	body.free = {{false, true, false, false}};
	s.bodies.push_back(body);
	SolverOptions o = options;
	o.particles = false;
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, o, pool ? pool->Threads() : 1, error)) { r.error = 1e300; return r; }
	const double ma = threeD ? 0.5*rho*V*(b*b*b + 2*a*a*a)/(b*b*b - a*a*a) : rho*Pi*a*a*(b*b + a*a)/(b*b - a*a)*h;
	r.exact = -(mass - rho*V)*g/(mass + ma);
	bool first = true;
	if (!m.Run(duration, 1.0/30, [&](double, const StepStats& st) {
		if (first) { r.acceleration = m.bodies[0].state.v[1]/st.dt; first = false; }
		r.maxIterations = std::max(r.maxIterations, st.pressure.iterations);
		++r.steps;
	}, error)) { r.error = 1e300; return r; }
	r.error = r.acceleration/r.exact - 1;
	r.drop = m.bodies[0].state.x[1] - c[1];
	r.dropModel = 0.5*r.exact*duration*duration;
	r.seconds = Now() - begin;
	return r;
}

// ------------------------------------------------------------------ bobbing (m7b)

namespace {
// The half-tank potential problem of BoxHeaveFrequency: A(kappa), kappa = omega^2 / g.
double BoxHeaveIntegral(double L, double boxWidth, double D, double d, int cells, double kappa)
{
	const double delta = 0.5*L/cells;
	const int nx = cells, ny = static_cast<int>(std::lround(D/delta));
	const int bx = static_cast<int>(std::lround(0.5*boxWidth/delta)), by = static_cast<int>(std::lround((D - d)/delta));
	// Fluid cells: all but the box [0, bx) x [by, ny).
	auto fluid = [&](int i, int j) { return i >= 0 && i < nx && j >= 0 && j < ny && !(i < bx && j >= by); };
	std::vector<int> id(std::size_t(nx)*std::size_t(ny), -1);
	int n = 0;
	for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) if (fluid(i, j)) id[std::size_t(i) + std::size_t(nx)*std::size_t(j)] = n++;
	const double robin = (1 + 0.5*kappa*delta)/(1 - 0.5*kappa*delta);
	// Operator (symmetric, scaled by delta^2): (K phi)_c = sum over fluid neighbours (phi_c - phi_nb)
	// + at the free surface (1 - robin) phi_c; rhs: delta (unit flux) under the box bottom.
	std::vector<double> diag(std::size_t(n), 0.0), rhs(std::size_t(n), 0.0);
	std::vector<std::array<int,4>> nb(static_cast<std::size_t>(n));
	for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
		const int c = id[std::size_t(i) + std::size_t(nx)*std::size_t(j)];
		if (c < 0) continue;
		const int di[4] = {-1, 1, 0, 0}, dj[4] = {0, 0, -1, 1};
		for (int k = 0; k < 4; ++k) {
			const int ii = i + di[k], jj = j + dj[k];
			nb[std::size_t(c)][std::size_t(k)] = -1;
			if (fluid(ii, jj)) { nb[std::size_t(c)][std::size_t(k)] = id[std::size_t(ii) + std::size_t(nx)*std::size_t(jj)]; diag[std::size_t(c)] += 1; continue; }
			if (k == 3 && jj == ny && i >= bx) { diag[std::size_t(c)] += 1 - robin; continue; }   // free surface
			if (k == 3 && jj == by && i < bx) rhs[std::size_t(c)] += delta;                       // the box bottom: d phi / dy = 1
			// other boundaries: Neumann (no term)
		}
	}
	auto apply = [&](const std::vector<double>& x, std::vector<double>& y) {
		for (int c = 0; c < n; ++c) {
			double s = diag[std::size_t(c)]*x[std::size_t(c)];
			for (int k = 0; k < 4; ++k) { const int q = nb[std::size_t(c)][std::size_t(k)]; if (q >= 0) s -= x[std::size_t(q)]; }
			y[std::size_t(c)] = s;
		}
	};
	// MINRES (Paige and Saunders 1975; the minres.m recurrences) with the
	// diagonal preconditioner max(1, |diag|): the operator is symmetric and
	// indefinite (the near-constant mode is negative for kappa > 0).
	const std::size_t N = std::size_t(n);
	std::vector<double> x(N, 0.0), r1 = rhs, r2 = rhs, y(N), v(N), wv(N, 0.0), w1(N, 0.0), w2(N, 0.0), M(N);
	for (std::size_t c = 0; c < N; ++c) M[c] = std::max(1.0, std::fabs(diag[c]));
	for (std::size_t c = 0; c < N; ++c) y[c] = r1[c]/M[c];
	double beta1 = 0;
	for (std::size_t c = 0; c < N; ++c) beta1 += r1[c]*y[c];
	beta1 = std::sqrt(beta1);
	double oldb = 0, beta = beta1, dbar = 0, epsln = 0, phibar = beta1, cs = -1, sn = 0;
	for (int itn = 1; itn <= 400000 && beta1 > 0; ++itn) {
		const double sc = 1/beta;
		for (std::size_t c = 0; c < N; ++c) v[c] = sc*y[c];
		apply(v, y);
		if (itn >= 2) for (std::size_t c = 0; c < N; ++c) y[c] -= beta/oldb*r1[c];
		double alfa = 0;
		for (std::size_t c = 0; c < N; ++c) alfa += v[c]*y[c];
		for (std::size_t c = 0; c < N; ++c) y[c] -= alfa/beta*r2[c];
		r1.swap(r2);
		r2 = y;
		for (std::size_t c = 0; c < N; ++c) y[c] = r2[c]/M[c];
		oldb = beta;
		beta = 0;
		for (std::size_t c = 0; c < N; ++c) beta += r2[c]*y[c];
		beta = std::sqrt(std::max(0.0, beta));
		const double oldeps = epsln;
		const double dlt = cs*dbar + sn*alfa, gbar = sn*dbar - cs*alfa;
		epsln = sn*beta;
		dbar = -cs*beta;
		const double gamma = std::max(1e-300, std::sqrt(gbar*gbar + beta*beta));
		cs = gbar/gamma;
		sn = beta/gamma;
		const double phi = cs*phibar;
		phibar = sn*phibar;
		w1.swap(w2);
		w2.swap(wv);
		for (std::size_t c = 0; c < N; ++c) { wv[c] = (v[c] - oldeps*w1[c] - dlt*w2[c])/gamma; x[c] += phi*wv[c]; }
		if (std::fabs(phibar) < 1e-13*beta1 || beta == 0) break;
	}
	// A over the full width: 2 x sum over the cells under the box of phi at the bottom face.
	double A = 0;
	for (int i = 0; i < bx; ++i) {
		const int c = id[std::size_t(i) + std::size_t(nx)*std::size_t(by - 1)];
		if (c >= 0) A += (x[std::size_t(c)] + 0.5*delta)*delta;
	}
	return 2*A;
}
} // namespace

double BoxHeaveManometer(double L, double w, double D, double d)
{
	const double s = 0.5*(L - w), c = D - d, g = 9.81;
	const double inertia = w*d + w*w*w/(12*c) + w*w*d/(2*s);   // / rho
	return std::sqrt(g*w*(1 + w/(2*s))/inertia);
}

double BoxHeaveFrequency(double L, double w, double D, double d, int cells)
{
	const double g = 9.81, m = w*d;   // per unit length / rho
	auto f = [&](double omega) { const double A = BoxHeaveIntegral(L, w, D, d, cells, omega*omega/g); return omega*omega*(m + A) - g*w; };
	double w0 = BoxHeaveManometer(L, w, D, d), w1 = 1.05*w0, f0 = f(w0), f1 = f(w1);
	for (int it = 0; it < 30 && std::fabs(f1) > 1e-12*g*w && f1 != f0; ++it) {
		const double w2 = w1 - f1*(w1 - w0)/(f1 - f0);
		w0 = w1; f0 = f1; w1 = w2; f1 = f(w1);
	}
	return w1;
}

BobResult RunBoxBobbing(int cells, double L, double w, double D, double d, double offset, double periods,
	const SolverOptions& options, Pool* pool)
{
	const double begin = Now();
	BobResult r;
	const double h = L/cells;
	LiquidSetup s = Tank2D(cells, static_cast<int>(std::lround(1.0/h)), h);
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, D, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	BodyShape box;
	box.type = BodyShape::Type::Box;
	box.size = {{w, 2*d, 4*h}};
	const double mass = s.density*w*d*h;   // density ratio 1/2 in the slab
	const double yEq = D - d + d;          // the centre: the bottom at D - d
	LiquidBody b = MakeBody(box, BodyMotion::Dynamic, mass, {{0.5*L, yEq - offset, 0.0}}, s.gravity);
	b.free = {{false, true, false, false}};
	s.bodies.push_back(b);
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) { r.omega = -1; return r; }
	const double period = 2*Pi/BoxHeaveManometer(L, w, D, d);
	std::vector<double> t{0}, y{yEq - offset};
	if (!m.Run(periods*period, 1.0/30, [&](double time, const StepStats&) { t.push_back(time); y.push_back(m.bodies[0].state.x[1]); ++r.steps; }, error)) {
		r.omega = -1; return r;
	}
	// The frequency: least squares of y = c0 + c1 t + a cos(omega t) + b sin(omega t)
	// over the run, maximised over omega by golden-section search around the
	// zero-crossing estimate (crossings of the mean).
	double mean = 0;
	for (double v : y) mean += v;
	mean /= double(y.size());
	std::vector<double> ups;
	for (std::size_t k = 1; k < y.size(); ++k)
		if (y[k - 1] < mean && y[k] >= mean) ups.push_back(t[k - 1] + (mean - y[k - 1])/(y[k] - y[k - 1])*(t[k] - t[k - 1]));
	r.crossings = int(ups.size());
	auto fit = [&](double om, double* amp) {
		// Normal equations of the 4 basis functions (solved by Gaussian elimination).
		double A[4][5] = {};
		for (std::size_t k = 0; k < y.size(); ++k) {
			const double f[4] = {1, t[k], std::cos(om*t[k]), std::sin(om*t[k])};
			for (int i = 0; i < 4; ++i) { for (int j = 0; j < 4; ++j) A[i][j] += f[i]*f[j]; A[i][4] += f[i]*y[k]; }
		}
		for (int i = 0; i < 4; ++i) {
			int p = i;
			for (int q = i + 1; q < 4; ++q) if (std::fabs(A[q][i]) > std::fabs(A[p][i])) p = q;
			for (int j = 0; j < 5; ++j) std::swap(A[i][j], A[p][j]);
			for (int q = 0; q < 4; ++q) if (q != i) { const double f = A[q][i]/A[i][i]; for (int j = i; j < 5; ++j) A[q][j] -= f*A[i][j]; }
		}
		double c[4];
		for (int i = 0; i < 4; ++i) c[i] = A[i][4]/A[i][i];
		double res = 0;
		for (std::size_t k = 0; k < y.size(); ++k) {
			const double e = y[k] - (c[0] + c[1]*t[k] + c[2]*std::cos(om*t[k]) + c[3]*std::sin(om*t[k]));
			res += e*e;
		}
		if (amp) *amp = std::sqrt(c[2]*c[2] + c[3]*c[3]);
		return res;
	};
	if (ups.size() >= 2) {
		const double guess = 2*Pi*double(ups.size() - 1)/(ups.back() - ups.front());
		double lo = 0.8*guess, hi = 1.2*guess;
		const double gr = 0.5*(std::sqrt(5.0) - 1);
		double x1 = hi - gr*(hi - lo), x2 = lo + gr*(hi - lo), f1 = fit(x1, nullptr), f2 = fit(x2, nullptr);
		for (int it = 0; it < 80; ++it) {
			if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - gr*(hi - lo); f1 = fit(x1, nullptr); }
			else { lo = x1; x1 = x2; f1 = f2; x2 = lo + gr*(hi - lo); f2 = fit(x2, nullptr); }
		}
		r.omega = 0.5*(lo + hi);
	}
	// Decay: the amplitude ratio of the last and first full periods.
	if (ups.size() >= 3) {
		auto amp = [&](double a, double b2) { double lo2 = 1e300, hi2 = -1e300; for (std::size_t k = 0; k < y.size(); ++k) if (t[k] >= a && t[k] <= b2) { lo2 = std::min(lo2, y[k]); hi2 = std::max(hi2, y[k]); } return hi2 - lo2; };
		r.decay = amp(ups[ups.size() - 2], ups.back())/amp(ups[0], ups[1]);
	}
	r.seconds = Now() - begin;
	return r;
}

double FloatingDepth(bool threeD, double R, double ratio)
{
	const double target = ratio*RoundVolume(threeD, R, 1);
	double lo = 0, hi = 2*R;
	for (int it = 0; it < 200; ++it) {
		const double mid = 0.5*(lo + hi);
		if (Submerged(threeD, R, 1, mid) < target) lo = mid; else hi = mid;
	}
	return 0.5*(lo + hi);
}

namespace {
// The floating tank: [0, 1]^d filled to 0.5, a round body of radius 0.15.
constexpr double FloatEta = 0.5, FloatR = 0.15;
LiquidSetup FloatTank(int cells, bool threeD)
{
	LiquidSetup s = UnitBox(cells, threeD);
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, FloatEta, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	return s;
}

// The net vertical force on a kinematic body held at rest at height y (one step of 1e-4 s).
double HeldForce(int cells, bool threeD, double ratio, double y, double shift, const SolverOptions& options, Pool* pool, bool& ok)
{
	LiquidSetup s = FloatTank(cells, threeD);
	const double h = s.grid.h;
	const double mass = ratio*s.density*RoundVolume(threeD, FloatR, h);
	const V3 x{{0.5 + shift*h, y, threeD ? 0.5 + shift*h : 0.0}};
	s.bodies.push_back(MakeBody(RoundBody(threeD, FloatR, h), BodyMotion::Prescribed, mass, x, s.gravity));
	SolverOptions o = options;
	o.particles = false;
	LiquidModel m;
	std::string error;
	StepStats st;
	const double dt = 1e-4;
	ok = m.Initialize(s, o, pool ? pool->Threads() : 1, error) && m.Step(dt, st, error);
	if (!ok) return 0;
	return m.bodyImpulse[0][1]/dt - mass*9.81;
}
} // namespace

FloatStatics RunFloatingStatics(int cells, bool threeD, double ratio, double shift, const SolverOptions& options, Pool* pool)
{
	const double start = Now();
	FloatStatics r;
	const double h = 1.0/cells, R = FloatR;
	const double mg = ratio*1000*RoundVolume(threeD, R, h)*9.81;
	r.yExact = FloatEta + R - FloatingDepth(threeD, R, ratio);
	bool ok = true;
	double y0 = r.yExact, f0 = HeldForce(cells, threeD, ratio, y0, shift, options, pool, ok);
	++r.solves;
	r.imbalance = f0/mg;
	double y1 = r.yExact + 0.02*R, f1 = HeldForce(cells, threeD, ratio, y1, shift, options, pool, ok);
	++r.solves;
	for (int it = 0; it < 12 && ok && std::fabs(f1) > 1e-11*mg && f1 != f0; ++it) {
		const double y2 = y1 - f1*(y1 - y0)/(f1 - f0);
		y0 = y1; f0 = f1;
		y1 = y2;
		f1 = HeldForce(cells, threeD, ratio, y1, shift, options, pool, ok);
		++r.solves;
	}
	if (!ok) { r.offset = 1e300; return r; }
	r.yDiscrete = y1;
	r.offset = (y1 - r.yExact)/R;
	r.seconds = Now() - start;
	return r;
}

FloatDynamics RunFloatingDynamics(int cells, bool threeD, double ratio, double startOffset, double duration, bool free,
	const SolverOptions& options, Pool* pool)
{
	const double begin = Now();
	FloatDynamics r;
	LiquidSetup s = FloatTank(cells, threeD);
	const double h = s.grid.h, R = FloatR, gy = s.gravity[1];
	const double V = RoundVolume(threeD, R, h), mass = ratio*s.density*V;
	const double d = FloatingDepth(threeD, R, ratio), yExact = FloatEta + R - d, y0 = yExact + startOffset*R;
	// The level after the release: the liquid outside the body keeps its volume.
	const double area = threeD ? 1.0 : h;
	const double etaFinal = FloatEta + (ratio*V - Submerged(threeD, R, h, FloatEta - (y0 - R)))/area;
	r.yEq = etaFinal + R - d;
	const V3 x{{0.5, y0, threeD ? 0.5 : 0.0}};
	LiquidBody body = MakeBody(RoundBody(threeD, R, h), BodyMotion::Dynamic, mass, x, s.gravity);
	if (!free) body.free = {{false, true, false, false}};
	s.bodies.push_back(body);
	LiquidModel m;
	std::string error;
	if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) { r.finite = false; return r; }
	m.audit = true;
	const V3 p0 = m.FluidMomentum();
	const double v0 = m.bodies[0].state.v[1];
	double impulses = 0, walls = 0, gravityLiquid = 0, T = 0;
	r.t.push_back(0);
	r.y.push_back(y0);
	if (!m.Run(duration, 1.0/30, [&](double t, const StepStats& st) {
		impulses += m.bodyImpulse[0][1];
		walls += m.wallImpulse[1];
		gravityLiquid += st.dt*gy*m.FluidMass(1);
		T += st.dt;
		r.t.push_back(t);
		r.y.push_back(m.bodies[0].state.x[1]);
		const auto& v = m.bodies[0].state.v;
		r.maxSpeed = std::max(r.maxSpeed, std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]));
		r.maxIterations = std::max(r.maxIterations, st.pressure.iterations);
		++r.steps;
	}, error)) { r.finite = false; r.seconds = Now() - begin; return r; }
	const double mgT = mass*std::fabs(gy)*T;
	const double dpBody = mass*(m.bodies[0].state.v[1] - v0);
	r.bodyAudit = std::fabs(dpBody - mass*gy*T - impulses)/mgT;
	const double dpLiquid = m.FluidMomentum()[1] - p0[1];
	const double residual = std::fabs(dpLiquid + dpBody - (gravityLiquid + mass*gy*T) - walls);
	r.totalAuditBody = residual/mgT;
	r.totalAudit = residual/(std::fabs(gravityLiquid) + mgT);
	// Mean over whole periods: between the first and last upward crossings of the overall mean.
	double mean = 0;
	for (double y : r.y) mean += y;
	mean /= double(r.y.size());
	std::vector<double> ups;
	for (std::size_t k = 1; k < r.y.size(); ++k)
		if (r.y[k - 1] < mean && r.y[k] >= mean) ups.push_back(r.t[k - 1] + (mean - r.y[k - 1])/(r.y[k] - r.y[k - 1])*(r.t[k] - r.t[k - 1]));
	if (ups.size() >= 2) {
		double s2 = 0, w = 0;
		for (std::size_t k = 1; k < r.y.size(); ++k) {
			const double tm = 0.5*(r.t[k] + r.t[k - 1]);
			if (tm < ups.front() || tm > ups.back()) continue;
			const double dt = r.t[k] - r.t[k - 1];
			s2 += 0.5*(r.y[k] + r.y[k - 1])*dt;
			w += dt;
		}
		mean = s2/w;
		r.period = (ups.back() - ups.front())/double(ups.size() - 1);
	}
	r.meanOffset = (mean - r.yEq)/R;
	double worst = 0, late = 0;
	for (std::size_t k = 0; k < r.y.size(); ++k) {
		worst = std::max(worst, std::fabs(r.y[k] - mean));
		if (r.t[k] >= 0.5*duration) late = std::max(late, std::fabs(r.y[k] - mean));
	}
	r.growth = worst/std::max(1e-300, std::fabs(y0 - mean));
	r.lateGrowth = late/std::max(1e-300, std::fabs(y0 - mean));
	for (double y : r.y) if (!std::isfinite(y)) r.finite = false;
	r.seconds = Now() - begin;
	return r;
}

NeutralResult RunNeutralBody(int cells, bool threeD, double duration, const SolverOptions& options, Pool* pool)
{
	const double begin = Now();
	NeutralResult r;
	for (int pass = 0; pass < 2; ++pass) {
		LiquidSetup s = UnitBox(cells, threeD);
		Shape fill;
		fill.type = Shape::Type::Plane;
		fill.point = {{0, 0.9, 0}};
		fill.axis = {{0, 1, 0}};
		s.liquid.push_back(fill);
		const double h = s.grid.h, R = FloatR;
		const V3 x{{0.5, std::round(0.45*cells)*h, threeD ? 0.5 : 0.0}};
		const double V = RoundVolume(threeD, R, h);
		// Pass 0: the discrete neutral mass (from the closed y-faces at this pose); pass 1: rho V.
		double mass = s.density*V;
		if (pass == 0) {
			LiquidSetup probe = s;
			probe.bodies.push_back(MakeBody(RoundBody(threeD, R, h), BodyMotion::Prescribed, 1, x, s.gravity));
			LiquidModel pm;
			std::string e;
			SolverOptions po = options;
			po.particles = false;
			if (!pm.Initialize(probe, po, 1, e)) { r.bodySpeed = 1e300; return r; }
			double vy = 0;
			for (const BodyFace& f : pm.Geometry().Faces()[0]) if (f.axis == 1) vy += f.closed;
			vy *= h*h*h;
			r.volumeError = vy/V - 1;
			mass = s.density*vy;
		}
		s.bodies.push_back(MakeBody(RoundBody(threeD, R, h), BodyMotion::Dynamic, mass, x, s.gravity));
		LiquidModel m;
		std::string error;
		if (!m.Initialize(s, options, pool ? pool->Threads() : 1, error)) { r.bodySpeed = 1e300; return r; }
		double T = 0;
		if (!m.Run(duration, 1.0/30, [&](double, const StepStats& st) {
			T += st.dt;
			if (pass == 0) {
				const auto& v = m.bodies[0].state.v;
				const V3 w = m.bodies[0].Omega();
				r.bodySpeed = std::max(r.bodySpeed, std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) +
					std::sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2])*R);
				r.liquidSpeed = std::max(r.liquidSpeed, m.MaxSpeed());
				++r.steps;
			}
		}, error)) { r.bodySpeed = 1e300; return r; }
		if (pass == 0) { r.bodySpeed /= std::sqrt(9.81*h); r.liquidSpeed /= std::sqrt(9.81*h); }
		else r.exactMassAcceleration = m.bodies[0].state.v[1]/T/9.81;
	}
	r.seconds = Now() - begin;
	return r;
}

} } }
