//////////////////////////////////////////////////////////////////////
//
//  ClothValidation.cpp - Analytic validation cases for the cloth model
//  (simulation M3a)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothValidation.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace RISE { namespace Simulation { namespace Cloth {

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double Gravity = 9.81;

CaseResult Failed(const std::string& why)
{
	CaseResult r;
	r.ok = false;
	r.failure = why;
	return r;
}

std::string Format(const char* fmt, double a, double b = 0, double c = 0, double d = 0)
{
	char buffer[256];
	std::snprintf(buffer, sizeof buffer, fmt, a, b, c, d);
	return buffer;
}

// Plane-stress orthotropic stiffness (as ClothSystem builds it).
void Stiffness(const Material& m, double& c11, double& c12, double& c22)
{
	const double nu21 = m.poisson*m.stretchWeft/m.stretchWarp, den = 1 - m.poisson*nu21;
	c11 = m.stretchWarp/den; c22 = m.stretchWeft/den; c12 = m.poisson*m.stretchWeft/den;
}

bool Build(const SheetSpec& spec, const ModelConfig& config, ClothSystem& system, std::string& error)
{
	Sheet sheet;
	return BuildSheet(spec, config.material, sheet, error) && system.Initialize(sheet, config, error);
}

// Zero crossings of a uniformly sampled series (cubic interpolation where
// four samples are available, linear at the ends); times in sample units.
std::vector<double> ZeroCrossings(const std::vector<double>& q)
{
	std::vector<double> out;
	for (std::size_t k = 0; k + 1 < q.size(); ++k) {
		if (!((q[k] > 0 && q[k+1] <= 0) || (q[k] < 0 && q[k+1] >= 0))) continue;
		if (q[k+1] == 0) { out.push_back(static_cast<double>(k + 1)); ++k; continue; }
		double lo = 0, hi = 1;
		auto value = [&](double t) {
			if (k == 0 || k + 2 >= q.size()) return q[k] + (q[k+1] - q[k])*t;
			// Lagrange cubic through samples at -1, 0, 1, 2.
			const double ym = q[k-1], y0 = q[k], y1 = q[k+1], y2 = q[k+2];
			return ym*(-t*(t - 1)*(t - 2)/6) + y0*((t + 1)*(t - 1)*(t - 2)/2) +
				y1*(-(t + 1)*t*(t - 2)/2) + y2*((t + 1)*t*(t - 1)/6);
		};
		const double s0 = value(0);
		for (int it = 0; it < 80; ++it) {
			const double mid = 0.5*(lo + hi);
			if ((value(mid) > 0) == (s0 > 0)) lo = mid; else hi = mid;
		}
		out.push_back(static_cast<double>(k) + 0.5*(lo + hi));
	}
	return out;
}

// Free vibration from the given transverse shape; returns the modal series.
bool FreeVibration(ClothSystem& system, const std::vector<double>& shape, double amplitude, double dt, int steps,
	Scheme scheme, const NewtonOptions& newton, std::vector<double>& series, std::vector<double>* energy,
	StepStats& stats, std::string& error)
{
	const std::uint32_t n = system.Vertices();
	const auto& mass = system.GetSheet().mass;
	double norm = 0;
	for (std::uint32_t q = 0; q < n; ++q) norm += mass[q]*shape[q]*shape[q];
	const double staticEnergy = system.ElasticEnergy(system.x);
	for (std::uint32_t q = 0; q < n; ++q) if (!system.Fixed(q)) system.x[3*static_cast<std::size_t>(q) + 2] += amplitude*shape[q];
	auto record = [&]() {
		double s = 0;
		for (std::uint32_t q = 0; q < n; ++q) s += mass[q]*system.x[3*static_cast<std::size_t>(q) + 2]*shape[q];
		series.push_back(s/norm);
		if (energy) energy->push_back(system.KineticEnergy(system.v) + system.ElasticEnergy(system.x) - staticEnergy);
	};
	series.clear();
	if (energy) energy->clear();
	record();
	for (int k = 0; k < steps; ++k) {
		if (!system.Step(dt, scheme, newton, stats, error)) return false;
		record();
	}
	return true;
}

double MeasuredFrequency(const std::vector<double>& series, double dt)
{
	const auto c = ZeroCrossings(series);
	if (c.size() < 3) return 0;
	return static_cast<double>(c.size() - 1)/(2*(c.back() - c.front())*dt);
}

struct MembraneSetup {
	double a = 0, b = 0, tu = 0, tv = 0, sigma = 0, omega = 0, omegaDiscrete = 0;
	int cu = 0, cv = 0;
};

bool BuildMembrane(const CaseSettings& settings, int m, int n, ClothSystem& system, MembraneSetup& s,
	std::vector<double>& shape, std::string& error)
{
	Material mat = settings.material;
	mat.bendWarp = mat.bendWeft = 0;
	const double a0 = 1.0, stretch = 1.003; // b = 0.75 a: (m,n) modes well separated
	s.cu = settings.resolution;
	s.cv = std::max(2, (3*settings.resolution + 2)/4);
	SheetSpec spec;
	// Square cells: the v extent follows the rounded cell count (0.75 m when 4 divides the resolution).
	spec.sizeU = a0;
	spec.sizeV = a0*static_cast<double>(s.cv)/static_cast<double>(s.cu);
	spec.verticesU = static_cast<std::uint32_t>(s.cu + 1); spec.verticesV = static_cast<std::uint32_t>(s.cv + 1);
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 1, 0}};
	spec.stretchU = spec.stretchV = stretch;
	ModelConfig config;
	config.material = mat;
	config.gravity = {{0, 0, 0}};
	config.damping = false;
	PinGroup boundary;
	for (std::uint32_t j = 0; j < spec.verticesV; ++j) for (std::uint32_t i = 0; i < spec.verticesU; ++i)
		if (i == 0 || j == 0 || i + 1 == spec.verticesU || j + 1 == spec.verticesV) boundary.vertices.push_back(i + spec.verticesU*j);
	config.pins.push_back(boundary);
	if (!Build(spec, config, system, error)) return false;
	double c11, c12, c22;
	Stiffness(mat, c11, c12, c22);
	const double e = 0.5*(stretch*stretch - 1);
	s.tu = (c11 + c12)*e; s.tv = (c12 + c22)*e;       // Cauchy tension = S for equibiaxial stretch
	s.sigma = mat.density/(stretch*stretch);
	s.a = stretch*spec.sizeU; s.b = stretch*spec.sizeV;
	const double km = static_cast<double>(m)/s.a, kn = static_cast<double>(n)/s.b;
	s.omega = Pi*std::sqrt((s.tu*km*km + s.tv*kn*kn)/s.sigma);
	const double hx = s.a/static_cast<double>(s.cu), hy = s.b/static_cast<double>(s.cv);
	const double su = std::sin(Pi*static_cast<double>(m)/(2.0*s.cu)), sv = std::sin(Pi*static_cast<double>(n)/(2.0*s.cv));
	s.omegaDiscrete = std::sqrt(4/s.sigma*(s.tu*su*su/(hx*hx) + s.tv*sv*sv/(hy*hy)));
	const std::uint32_t count = system.Vertices();
	shape.assign(count, 0.0);
	for (std::uint32_t q = 0; q < count; ++q) {
		if (system.Fixed(q)) continue;
		const double x = system.x[3*static_cast<std::size_t>(q)], y = system.x[3*static_cast<std::size_t>(q) + 1];
		shape[q] = std::sin(Pi*static_cast<double>(m)*x/s.a)*std::sin(Pi*static_cast<double>(n)*y/s.b);
	}
	return true;
}
} // namespace

// ---------------------------------------------------------------- references

bool ElasticaCantilever(double q, double rigidity, double length, double stretchStiffness, const std::vector<double>& s,
	std::vector<double>& x, std::vector<double>& drop, std::string& error)
{
	if (!(q > 0) || !(rigidity > 0) || !(length > 0) || stretchStiffness < 0) {
		error = "elastica needs positive load, rigidity and length";
		return false;
	}
	const double alpha = q*length*length*length/rigidity;
	// Axial stretch lambda from the tension N = q (L - s) sin(theta) (per unit
	// width) under the model's law N = E lambda (lambda^2 - 1)/2; 1 when inextensible.
	auto stretch = [&](double tension) {
		if (stretchStiffness == 0) return 1.0;
		double l = 1 + tension/stretchStiffness;
		for (int it = 0; it < 2; ++it) // Newton from 1 + N/E: quadratic, |N/E| < 1e-3 here
			l -= (stretchStiffness*l*(l*l - 1)/2 - tension)/(stretchStiffness*(3*l*l - 1)/2);
		return l;
	};
	// State (theta, theta', x, drop) in s/L; theta'' = -alpha (1 - s) lambda cos(theta),
	// x' = lambda cos(theta), drop' = lambda sin(theta) (curvature per reference length,
	// as the discrete rods measure it).
	const int steps = 8000;
	auto integrate = [&](double kappa0, const std::vector<double>* samples, std::vector<double>* xs, std::vector<double>* ds) {
		double y[4] = {0, kappa0, 0, 0};
		const double h = 1.0/steps;
		std::size_t next = 0;
		auto rhs = [&](double t, const double* st, double* d) {
			const double l = stretch(q*length*(1 - t)*std::sin(st[0]));
			d[0] = st[1];
			d[1] = -alpha*(1 - t)*l*std::cos(st[0]);
			d[2] = l*std::cos(st[0]);
			d[3] = l*std::sin(st[0]);
		};
		for (int k = 0; k < steps; ++k) {
			const double t = static_cast<double>(k)*h;
			// Emit any samples in [t, t + h) by an RK4 substep of the exact length.
			while (samples && next < samples->size() && (*samples)[next]/length < t + h - 1e-15) {
				const double dt = (*samples)[next]/length - t;
				double z[4] = {y[0], y[1], y[2], y[3]};
				if (dt > 0) {
					double k1[4], k2[4], k3[4], k4[4], w[4];
					rhs(t, z, k1);
					for (int i = 0; i < 4; ++i) w[i] = z[i] + 0.5*dt*k1[i];
					rhs(t + 0.5*dt, w, k2);
					for (int i = 0; i < 4; ++i) w[i] = z[i] + 0.5*dt*k2[i];
					rhs(t + 0.5*dt, w, k3);
					for (int i = 0; i < 4; ++i) w[i] = z[i] + dt*k3[i];
					rhs(t + dt, w, k4);
					for (int i = 0; i < 4; ++i) z[i] += dt/6*(k1[i] + 2*k2[i] + 2*k3[i] + k4[i]);
				}
				xs->push_back(z[2]*length);
				ds->push_back(z[3]*length);
				++next;
			}
			double k1[4], k2[4], k3[4], k4[4], w[4];
			rhs(t, y, k1);
			for (int i = 0; i < 4; ++i) w[i] = y[i] + 0.5*h*k1[i];
			rhs(t + 0.5*h, w, k2);
			for (int i = 0; i < 4; ++i) w[i] = y[i] + 0.5*h*k2[i];
			rhs(t + 0.5*h, w, k3);
			for (int i = 0; i < 4; ++i) w[i] = y[i] + h*k3[i];
			rhs(t + h, w, k4);
			for (int i = 0; i < 4; ++i) y[i] += h/6*(k1[i] + 2*k2[i] + 2*k3[i] + k4[i]);
		}
		while (samples && next < samples->size()) { xs->push_back(y[2]*length); ds->push_back(y[3]*length); ++next; }
		return y[1]; // theta'(1)
	};
	double lo = 0, hi = 0.5*alpha*1.01;
	for (int it = 0; it < 64; ++it) {
		const double mid = 0.5*(lo + hi);
		if (integrate(mid, nullptr, nullptr, nullptr) < 0) lo = mid; else hi = mid;
	}
	x.clear(); drop.clear();
	integrate(0.5*(lo + hi), &s, &x, &drop);
	return true;
}

bool ElasticCatenary(double L0, double span, double w0, double EA, const std::vector<double>& s,
	std::vector<double>& x, std::vector<double>& sag, double& horizontal, std::string& error)
{
	if (!(L0 > 0) || !(span > 0) || !(w0 > 0) || !(EA > 0)) { error = "catenary needs positive data"; return false; }
	auto stretch = [&](double tension) { // solve T = EA l (l^2 - 1)/2 for l >= 1
		double l = 1 + tension/EA;
		for (int it = 0; it < 60; ++it) {
			const double f = EA*l*(l*l - 1)/2 - tension, d = EA*(3*l*l - 1)/2;
			const double step = f/d;
			l -= step;
			if (std::fabs(step) < 1e-17) break;
		}
		return l;
	};
	const double va = 0.5*w0*L0;
	auto integrands = [&](double H, double si, double& dx, double& dz) {
		const double V = va - w0*si, T = std::sqrt(H*H + V*V), l = stretch(T);
		dx = l*H/T; dz = l*V/T;
	};
	const int sub = 4096; // Simpson intervals over [0, L0]
	auto spanOf = [&](double H) {
		double total = 0;
		const double h = L0/sub;
		for (int k = 0; k <= sub; ++k) {
			double dx, dz;
			integrands(H, h*k, dx, dz);
			const double w = (k == 0 || k == sub) ? 1 : (k % 2 ? 4 : 2);
			total += w*dx;
		}
		return total*h/3;
	};
	double lo = 1e-9*w0*L0, hi = 1e6*w0*L0;
	for (int it = 0; it < 200; ++it) {
		const double mid = std::sqrt(lo*hi);
		if (spanOf(mid) < span) lo = mid; else hi = mid;
		if (hi/lo - 1 < 1e-15) break;
	}
	const double H = 0.5*(lo + hi);
	horizontal = H;
	x.clear(); sag.clear();
	double px = 0, pz = 0, prev = 0;
	for (double target : s) {
		// Simpson from prev to target with an even number of intervals.
		const int m = 2*std::max(1, static_cast<int>(std::ceil((target - prev)/L0*sub/2)));
		const double h = (target - prev)/m;
		double ax = 0, az = 0;
		for (int k = 0; k <= m && h > 0; ++k) {
			double dx, dz;
			integrands(H, prev + h*k, dx, dz);
			const double w = (k == 0 || k == m) ? 1 : (k % 2 ? 4 : 2);
			ax += w*dx; az += w*dz;
		}
		px += ax*h/3; pz += az*h/3;
		prev = target;
		x.push_back(px); sag.push_back(pz);
	}
	return true;
}

double PredictedEnergyRatio(Scheme scheme, double omegaDt, double stepsPerPeriod)
{
	switch (scheme) {
	case Scheme::BackwardEuler:
		return std::pow(1/(1 + omegaDt*omegaDt), stepsPerPeriod);
	case Scheme::BDF2: {
		const std::complex<double> z(0, omegaDt);
		const std::complex<double> zeta = (2.0 + std::sqrt(1.0 + 2.0*z))/(3.0 - 2.0*z);
		return std::pow(std::norm(zeta), stepsPerPeriod);
	}
	case Scheme::ImplicitMidpoint: break;
	}
	return 1;
}

double ObservedOrder(double coarse, double fine)
{
	return std::log(coarse/fine)/std::log(2.0);
}

// ---------------------------------------------------------------- cases

CaseResult RunCatenary(const CaseSettings& settings)
{
	Material mat = settings.material;
	mat.bendWarp = mat.bendWeft = 0;
	const int segments = settings.resolution;
	const double L0 = 1.0, span = 0.8, width = 0.05;
	SheetSpec spec;
	spec.sizeU = L0; spec.sizeV = width;
	spec.verticesU = static_cast<std::uint32_t>(segments + 1); spec.verticesV = 3;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 1, 0}};
	spec.stretchU = span/L0;
	ModelConfig config;
	config.material = mat;
	config.gravity = {{0, 0, -Gravity}};
	config.damping = false;
	PinGroup ends;
	for (std::uint32_t j = 0; j < 3; ++j) { ends.vertices.push_back(spec.verticesU*j); ends.vertices.push_back(spec.verticesU*j + segments); }
	config.pins.push_back(ends);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	// Initial guess: a parabola of roughly the right arc length.
	const double f = std::sqrt(3*span*(L0 - span)/8);
	const auto& sheet = system.GetSheet();
	for (std::uint32_t q = 0; q < system.Vertices(); ++q) {
		if (system.Fixed(q)) continue;
		const double xx = sheet.materialU[q]*span/L0;
		system.x[3*static_cast<std::size_t>(q)] = xx;
		system.x[3*static_cast<std::size_t>(q) + 2] = -4*f*xx*(span - xx)/(span*span);
	}
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-9*Gravity;
	CaseResult r;
	r.resolution = segments;
	if (!system.SolveStatic(newton, r.stats, error, 0.05, 6)) return Failed(error);
	std::vector<double> s, xr, zr;
	for (int i = 0; i <= segments; ++i) s.push_back(sheet.materialU[static_cast<std::size_t>(i)]);
	double H = 0;
	if (!ElasticCatenary(L0, span, mat.density*Gravity*width, mat.stretchWarp*width, s, xr, zr, H, error)) return Failed(error);
	const double sagRef = zr[static_cast<std::size_t>(segments/2)];
	double worst = 0, mid = 0;
	for (std::uint32_t q = 0; q < system.Vertices(); ++q) {
		const std::size_t i = q % spec.verticesU;
		const double dx = system.x[3*static_cast<std::size_t>(q)] - xr[i], dz = -system.x[3*static_cast<std::size_t>(q) + 2] - zr[i];
		worst = std::max(worst, std::sqrt(dx*dx + dz*dz));
		if (static_cast<int>(i) == segments/2) mid += -system.x[3*static_cast<std::size_t>(q) + 2]/3;
	}
	r.ok = true;
	r.error = worst/sagRef;
	r.measured = mid;
	r.reference = sagRef;
	r.secondary = H;
	r.detail = Format("max position error / sag %.3e; midspan sag %.6f m vs %.6f m; H %.5f N", r.error, mid, sagRef, H);
	return r;
}

CaseResult RunCantilever(const CaseSettings& settings, bool weft)
{
	const Material& mat = settings.material;
	const double B = weft ? mat.bendWeft : mat.bendWarp;
	const double q = mat.density*Gravity;
	if (!(B > 0)) return Failed("cantilever needs bending rigidity");
	const double bendingLength = std::cbrt(B/q), L = 2*bendingLength, width = L/4;
	const int segments = settings.resolution;
	const double h = L/segments;
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	if (!weft) {
		spec.sizeU = L + h; spec.verticesU = static_cast<std::uint32_t>(segments + 2);
		spec.sizeV = width; spec.verticesV = 3;
		spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 1, 0}};
		spec.origin = {{-h, -0.5*width, 0}};
	} else {
		spec.sizeV = L + h; spec.verticesV = static_cast<std::uint32_t>(segments + 2);
		spec.sizeU = width; spec.verticesU = 3;
		spec.axisU = {{0, -1, 0}}; spec.axisV = {{1, 0, 0}};
		spec.origin = {{-h, 0.5*width, 0}};
	}
	ModelConfig config;
	config.material = mat;
	config.gravity = {{0, 0, -Gravity}};
	config.damping = false;
	PinGroup clamp;
	for (std::uint32_t k = 0; k < 2; ++k) for (std::uint32_t c = 0; c < 3; ++c)
		clamp.vertices.push_back(weft ? c + spec.verticesU*k : k + spec.verticesU*c);
	config.pins.push_back(clamp);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-9*Gravity;
	CaseResult r;
	r.resolution = segments;
	if (!system.SolveStatic(newton, r.stats, error, 0.02, 8)) return Failed(error);
	std::vector<double> s, xr, dr;
	for (int k = 0; k <= segments; ++k) s.push_back(h*k);
	if (!ElasticaCantilever(q, B, L, weft ? mat.stretchWeft : mat.stretchWarp, s, xr, dr, error)) return Failed(error);
	double worst = 0, tip = 0;
	const std::uint32_t along = weft ? spec.verticesV : spec.verticesU;
	for (std::uint32_t k = 1; k < along; ++k) for (std::uint32_t c = 0; c < 3; ++c) {
		const std::uint32_t vtx = weft ? c + spec.verticesU*k : k + spec.verticesU*c;
		const std::size_t i = k - 1;
		const double dx = system.x[3*static_cast<std::size_t>(vtx)] - xr[i];
		const double dz = -system.x[3*static_cast<std::size_t>(vtx) + 2] - dr[i];
		worst = std::max(worst, std::sqrt(dx*dx + dz*dz));
		if (k + 1 == along) tip += -system.x[3*static_cast<std::size_t>(vtx) + 2]/3;
	}
	r.ok = true;
	r.error = worst/dr.back();
	r.measured = tip;
	r.reference = dr.back();
	r.secondary = std::fabs(tip - dr.back())/dr.back();
	r.detail = Format("max position error / tip drop %.3e; tip drop %.6e m vs %.6e m (rel %.2e)", r.error, tip, dr.back(), r.secondary);
	return r;
}

CaseResult RunMembraneMode(const CaseSettings& settings, int m, int n)
{
	ClothSystem system;
	MembraneSetup s;
	std::vector<double> shape, series;
	std::string error;
	if (!BuildMembrane(settings, m, n, system, s, shape, error)) return Failed(error);
	const double perPeriod = settings.stepsPerPeriod > 0 ? settings.stepsPerPeriod : 2.5*settings.resolution;
	const double period = 2*Pi/s.omega, dt = period/perPeriod;
	const int steps = static_cast<int>(std::ceil(settings.periods*perPeriod));
	const double amplitude = 1e-4*s.a;
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-7*s.omega*s.omega*amplitude;
	CaseResult r;
	r.resolution = settings.resolution;
	if (!FreeVibration(system, shape, amplitude, dt, steps, settings.scheme, newton, series, nullptr, r.stats, error))
		return Failed(error);
	const double f = MeasuredFrequency(series, dt), fRef = s.omega/(2*Pi);
	if (!(f > 0)) return Failed("no oscillation measured");
	r.ok = true;
	r.measured = f;
	r.reference = fRef;
	r.error = std::fabs(f - fRef)/fRef;
	r.secondary = s.omegaDiscrete/(2*Pi);
	r.detail = Format("f %.6f Hz vs %.6f Hz (semi-discrete %.6f Hz); %.0f steps/period", f, fRef, r.secondary, perPeriod);
	return r;
}

CaseResult RunPlate(const CaseSettings& settings, bool clamped)
{
	Material mat = settings.material;
	if (clamped) mat.bendWeft = mat.bendWarp; // the clamped reference is isotropic
	const double a = 0.2;
	const int cells = settings.resolution;
	const double h = a/cells;
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 1, 0}};
	const int ghost = clamped ? 1 : 0;
	spec.sizeU = spec.sizeV = a + 2*ghost*h;
	spec.verticesU = spec.verticesV = static_cast<std::uint32_t>(cells + 1 + 2*ghost);
	spec.origin = {{-ghost*h, -ghost*h, 0}};
	ModelConfig config;
	config.material = mat;
	config.gravity = {{0, 0, 0}};
	config.damping = false;
	PinGroup edge;
	const std::uint32_t nv = spec.verticesU, depth = static_cast<std::uint32_t>(1 + ghost);
	for (std::uint32_t j = 0; j < nv; ++j) for (std::uint32_t i = 0; i < nv; ++i)
		if (i < depth || j < depth || i + depth >= nv || j + depth >= nv) edge.vertices.push_back(i + nv*j);
	config.pins.push_back(edge);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	const std::uint32_t count = system.Vertices();
	std::vector<double> shape(count, 0.0);
	for (std::uint32_t q = 0; q < count; ++q) {
		if (system.Fixed(q)) continue;
		const double x = system.x[3*static_cast<std::size_t>(q)], y = system.x[3*static_cast<std::size_t>(q) + 1];
		const double sx = std::sin(Pi*x/a), sy = std::sin(Pi*y/a);
		shape[q] = clamped ? sx*sx*sy*sy : sx*sy;
	}
	// Inverse iteration on the linearised operator at the flat state removes
	// the higher modes the analytic shape contains (they would bias zero crossings).
	CaseResult r;
	r.resolution = cells;
	BlockMatrix k;
	system.ElasticHessian(system.x, k);
	const auto& mass = system.GetSheet().mass;
	for (int it = 0; it < 4; ++it) {
		std::vector<double> rhs(3*static_cast<std::size_t>(count), 0.0), y;
		for (std::uint32_t q = 0; q < count; ++q) if (!system.Fixed(q)) rhs[3*static_cast<std::size_t>(q) + 2] = mass[q]*shape[q];
		system.Solve(k, rhs, y, settings.newton, true, r.stats);
		double big = 0;
		for (std::uint32_t q = 0; q < count; ++q) big = std::max(big, std::fabs(y[3*static_cast<std::size_t>(q) + 2]));
		if (!(big > 0)) return Failed("plate inverse iteration failed");
		for (std::uint32_t q = 0; q < count; ++q) shape[q] = system.Fixed(q) ? 0 : y[3*static_cast<std::size_t>(q) + 2]/big;
	}
	const double sigma = mat.density;
	const double sb = std::sqrt(mat.bendWarp) + std::sqrt(mat.bendWeft);
	const double omega = clamped ? ClampedSquarePlateLambda2/(a*a)*std::sqrt(mat.bendWarp/sigma) :
		Pi*Pi/(a*a)*sb/std::sqrt(sigma);
	const double perPeriod = settings.stepsPerPeriod > 0 ? settings.stepsPerPeriod : 2.5*cells;
	const double dt = 2*Pi/omega/perPeriod;
	const int steps = static_cast<int>(std::ceil(settings.periods*perPeriod));
	// Amplitude far below the membrane-bending thickness sqrt(12 B / E) keeps the
	// von Karman frequency shift below 1e-6.
	const double thickness = std::sqrt(12*std::max(mat.bendWarp, mat.bendWeft)/std::min(mat.stretchWarp, mat.stretchWeft));
	const double amplitude = 2e-3*thickness;
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-5*omega*omega*amplitude;
	std::vector<double> series;
	if (!FreeVibration(system, shape, amplitude, dt, steps, settings.scheme, newton, series, nullptr, r.stats, error))
		return Failed(error);
	const double f = MeasuredFrequency(series, dt), fRef = omega/(2*Pi);
	if (!(f > 0)) return Failed("no oscillation measured");
	r.ok = true;
	r.measured = f;
	r.reference = fRef;
	r.error = std::fabs(f - fRef)/fRef;
	r.detail = Format("f %.6f Hz vs %.6f Hz; %.0f steps/period", f, fRef, perPeriod);
	return r;
}

CaseResult RunEnergyDrift(const CaseSettings& settings)
{
	ClothSystem system;
	MembraneSetup s;
	std::vector<double> shape, series, energy;
	std::string error;
	if (!BuildMembrane(settings, 1, 1, system, s, shape, error)) return Failed(error);
	const double perPeriod = settings.stepsPerPeriod > 0 ? settings.stepsPerPeriod : 2.5*settings.resolution;
	// An integer number of steps per discrete period keeps the fit window aligned.
	const double period = 2*Pi/s.omegaDiscrete, dt = period/perPeriod;
	const double periods = std::max(4.0, settings.periods);
	const int steps = static_cast<int>(std::ceil(periods*perPeriod));
	const double amplitude = 1e-4*s.a;
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-9*s.omega*s.omega*amplitude;
	CaseResult r;
	r.resolution = settings.resolution;
	if (!FreeVibration(system, shape, amplitude, dt, steps, settings.scheme, newton, series, &energy, r.stats, error))
		return Failed(error);
	// Least-squares slope of log energy over whole periods after the first.
	const int first = static_cast<int>(std::ceil(perPeriod));
	const int windowPeriods = static_cast<int>(std::floor((steps - first)/perPeriod));
	const int last = first + static_cast<int>(std::floor(windowPeriods*perPeriod));
	double sx = 0, sy = 0, sxx = 0, sxy = 0, count = 0;
	for (int k = first; k < last; ++k) {
		if (!(energy[static_cast<std::size_t>(k)] > 0)) return Failed("nonpositive oscillation energy");
		const double xk = k, yk = std::log(energy[static_cast<std::size_t>(k)]);
		sx += xk; sy += yk; sxx += xk*xk; sxy += xk*yk; count += 1;
	}
	const double slope = (count*sxy - sx*sy)/(count*sxx - sx*sx);
	const double ratio = std::exp(slope*perPeriod);
	const double predicted = PredictedEnergyRatio(settings.scheme, s.omegaDiscrete*dt, perPeriod);
	r.ok = true;
	r.measured = 1 - ratio;
	r.reference = 1 - predicted;
	r.error = std::fabs(r.measured - r.reference);
	r.detail = Format("energy loss per period %.4e (linear theory %.4e); %.0f steps/period", r.measured, r.reference, perPeriod);
	return r;
}

} } }
