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

} } }
