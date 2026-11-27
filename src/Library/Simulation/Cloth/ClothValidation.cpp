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
#include "ClothContact.h"

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

// ---------------------------------------------------------------- contact cases

double BarrierEquilibriumGap(double pressure, double kappa, double dhat)
{
	// |b'(z)| decreases from infinity at 0 to 0 at dhat: bisection.
	double lo = 1e-30*dhat, hi = dhat;
	for (int it = 0; it < 200; ++it) {
		const double mid = 0.5*(lo + hi);
		double b1 = 0;
		Barrier(mid, dhat, &b1, nullptr);
		if (kappa*std::fabs(b1) > pressure) lo = mid; else hi = mid;
	}
	return 0.5*(lo + hi);
}

namespace {
ModelConfig ContactConfig(const CaseSettings& settings, const ContactSettings& contact)
{
	ModelConfig config;
	config.material = settings.material;
	config.contact = ContactModel::Barrier;
	config.barrier = contact.barrier;
	config.contactThickness = contact.thickness;
	return config;
}
}

CaseResult RunContactRest(const CaseSettings& settings, const ContactSettings& contact)
{
	const Material& mat = settings.material;
	const double L = 0.2, width = 0.02, dhat = contact.barrier.activation, xi = contact.thickness;
	const double q = mat.density*Gravity;
	const double zStar = BarrierEquilibriumGap(q, contact.barrier.stiffness, dhat);
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.sizeU = L; spec.sizeV = width;
	spec.verticesU = static_cast<std::uint32_t>(settings.resolution + 1); spec.verticesV = 3;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{-0.5*L, xi + 0.5*dhat, -0.5*width}};
	ModelConfig config = ContactConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	config.damping = false;
	config.barrier.friction = 0;
	Collider plane;
	plane.type = Collider::Type::Plane; plane.a = {{0, 0, 0}}; plane.b = {{0, 1, 0}};
	config.colliders.push_back(plane);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-9*Gravity;
	newton.maxIterations = 100;
	CaseResult r;
	r.resolution = settings.resolution;
	// Frictionless: horizontal translation is free, so the solve keeps the last stage's mass term.
	if (!system.SolveStatic(newton, r.stats, error, 0.01, 10, false)) return Failed(error);
	double weight = 0;
	for (double m : system.GetSheet().mass) weight += m*Gravity;
	const V3 force = system.ContactForce(system.x, system.time);
	double worstGap = 0, smallest = std::numeric_limits<double>::infinity(), mean = 0;
	const std::uint32_t n = system.Vertices();
	for (std::uint32_t q2 = 0; q2 < n; ++q2) {
		const double z = system.x[3*static_cast<std::size_t>(q2) + 1] - xi;
		worstGap = std::max(worstGap, std::fabs(z - zStar));
		smallest = std::min(smallest, z);
		mean += z/n;
	}
	const double balance = std::fabs(force[1] - weight)/weight;
	r.ok = true;
	r.error = std::max(balance, worstGap/zStar);
	if (!(smallest > 0)) r.error = 1;
	r.measured = mean;
	r.reference = zStar;
	r.secondary = balance;
	r.detail = Format("contact force / weight - 1 = %.2e; gap z %.6e m (barrier law %.6e m); smallest gap %.3e m",
		(force[1] - weight)/weight, mean, zStar, smallest);
	return r;
}

bool ElasticaOverCylinder(double q, double rigidity, double halfLength, double rho, const std::vector<double>& s,
	std::vector<double>& x, std::vector<double>& y, double& liftOff, std::string& error)
{
	if (!(q > 0) || !(rigidity > 0) || !(halfLength > 0) || !(rho > 0)) { error = "elastica needs positive data"; return false; }
	const double alpha = q/rigidity;
	const int steps = 8000;
	// From the lift-off point sc: state (theta, theta', x, y); returns theta'(L/2).
	auto integrate = [&](double sc, const std::vector<double>* samples, std::vector<double>* xs, std::vector<double>* ys) {
		double st[4] = {sc/rho, 1/rho, rho*std::sin(sc/rho), rho*(1 - std::cos(sc/rho))};
		const double h = (halfLength - sc)/steps;
		auto rhs = [&](double sv, const double* z, double* d) {
			d[0] = z[1];
			d[1] = -alpha*(halfLength - sv)*std::cos(z[0]);
			d[2] = std::cos(z[0]);
			d[3] = std::sin(z[0]);
		};
		auto rk4 = [&](double sv, double* z, double dt) {
			double k1[4], k2[4], k3[4], k4[4], w[4];
			rhs(sv, z, k1);
			for (int i = 0; i < 4; ++i) w[i] = z[i] + 0.5*dt*k1[i];
			rhs(sv + 0.5*dt, w, k2);
			for (int i = 0; i < 4; ++i) w[i] = z[i] + 0.5*dt*k2[i];
			rhs(sv + 0.5*dt, w, k3);
			for (int i = 0; i < 4; ++i) w[i] = z[i] + dt*k3[i];
			rhs(sv + dt, w, k4);
			for (int i = 0; i < 4; ++i) z[i] += dt/6*(k1[i] + 2*k2[i] + 2*k3[i] + k4[i]);
		};
		std::size_t next = 0;
		if (samples) while (next < samples->size() && (*samples)[next] < sc) ++next;
		for (int k = 0; k < steps; ++k) {
			const double sv = sc + h*k;
			while (samples && next < samples->size() && (*samples)[next] < sv + h - 1e-15) {
				double z[4] = {st[0], st[1], st[2], st[3]};
				const double dt = (*samples)[next] - sv;
				if (dt > 0) rk4(sv, z, dt);
				(*xs)[next] = z[2]; (*ys)[next] = z[3];
				++next;
			}
			rk4(sv, st, h);
		}
		while (samples && next < samples->size()) { (*xs)[next] = st[2]; (*ys)[next] = st[3]; ++next; }
		return st[1];
	};
	// The physical root is the largest lift-off point at which theta'(L/2) = 0
	// (smaller roots bend the free part back up, against the moment's sign).
	const double top = std::min(halfLength, 0.5*3.14159265358979323846*rho);
	double lo = 0, hi = 0;
	bool found = false;
	double prev = integrate(top*(1 - 1e-9), nullptr, nullptr, nullptr);
	for (int k = 1999; k >= 1 && !found; --k) {
		const double sc = top*k/2000.0;
		const double f = integrate(sc, nullptr, nullptr, nullptr);
		if ((f > 0) != (prev > 0)) { lo = sc; hi = top*(k + 1)/2000.0; found = true; }
		prev = f;
	}
	if (!found) { error = "no wrapped elastica (the strip is too light or too short to lift off)"; return false; }
	const bool loPositive = integrate(lo, nullptr, nullptr, nullptr) > 0;
	for (int it = 0; it < 80; ++it) {
		const double mid = 0.5*(lo + hi);
		if ((integrate(mid, nullptr, nullptr, nullptr) > 0) == loPositive) lo = mid; else hi = mid;
	}
	liftOff = 0.5*(lo + hi);
	x.assign(s.size(), 0.0);
	y.assign(s.size(), 0.0);
	for (std::size_t k = 0; k < s.size(); ++k)
		if (s[k] <= liftOff) { x[k] = rho*std::sin(s[k]/rho); y[k] = rho*(1 - std::cos(s[k]/rho)); }
	integrate(liftOff, &s, &x, &y);
	return true;
}

CaseResult RunContactCylinder(const CaseSettings& settings, const ContactSettings& contact)
{
	const Material& mat = settings.material;
	const double B = mat.bendWarp, q = mat.density*Gravity;
	if (!(B > 0)) return Failed("the drape needs bending rigidity");
	const double lb = std::cbrt(B/q), R = 2*lb, half = 6*lb, width = half/5;
	const double dhat = contact.barrier.activation, xi = contact.thickness;
	const int segments = 2*((settings.resolution + 1)/2);
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.sizeU = 2*half; spec.sizeV = width;
	spec.verticesU = static_cast<std::uint32_t>(segments + 1); spec.verticesV = 3;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{-half, R + xi + 0.5*dhat, -0.5*width}};
	ModelConfig config = ContactConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	config.damping = false;
	config.barrier.friction = 0;
	Collider cyl;
	cyl.type = Collider::Type::Cylinder; cyl.a = {{0, 0, 0}}; cyl.b = {{0, 0, 1}}; cyl.radius = R;
	config.colliders.push_back(cyl);
	// Symmetry: the centre column is held at the top of the contact arc (x = 0,
	// height rho), which removes the free sliding along the arc (unstable on a
	// frictionless cylinder) and along the axis; by symmetry its reaction is
	// the contact force it replaces.
	const double rho = R + xi + BarrierEquilibriumGap(q, contact.barrier.stiffness, dhat);
	spec.origin[1] = rho;
	PinGroup top;
	for (std::uint32_t j = 0; j < 3; ++j) top.vertices.push_back(static_cast<std::uint32_t>(segments/2) + spec.verticesU*j);
	config.pins.push_back(top);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-8*Gravity;
	newton.maxIterations = 200;
	CaseResult r;
	r.resolution = segments;
	if (!system.SolveStatic(newton, r.stats, error, 0.01, 10, true)) return Failed(error);
	// Reference: the contact arc at the barrier's equilibrium gap for the weight.
	const double h = 2*half/segments;
	std::vector<double> sv;
	for (int k = 0; k <= segments/2; ++k) sv.push_back(h*k);
	std::vector<double> xr, yr;
	double liftOff = 0;
	if (!ElasticaOverCylinder(q, B, half, rho, sv, xr, yr, liftOff, error)) return Failed(error);
	double worst = 0;
	const std::uint32_t nu = spec.verticesU, centre = static_cast<std::uint32_t>(segments/2);
	for (std::uint32_t i = 0; i < nu; ++i) {
		const std::uint32_t vtx = i + nu;  // the middle row
		const std::size_t k = i >= centre ? i - centre : centre - i;
		const double sign = i >= centre ? 1.0 : -1.0;
		const double dx = system.x[3*static_cast<std::size_t>(vtx)] - sign*xr[k];
		const double dy = system.x[3*static_cast<std::size_t>(vtx) + 1] - (rho - yr[k]);
		worst = std::max(worst, std::sqrt(dx*dx + dy*dy));
	}
	r.ok = true;
	r.error = worst/half;
	r.measured = system.ContactGap(system.x, system.time);
	r.reference = liftOff;
	r.detail = Format("max centre-line error / (L/2) %.3e; lift-off at s = %.4f m of %.4f; smallest gap %.3e m",
		r.error, liftOff, half, r.measured);
	return r;
}

CaseResult RunContactIncline(const CaseSettings& settings, const ContactSettings& contact, double angle)
{
	const Material& mat = settings.material;
	const double size = 0.1, dhat = contact.barrier.activation, xi = contact.thickness, mu = contact.barrier.friction;
	const double gn = Gravity*std::cos(angle), gt = Gravity*std::sin(angle);
	const double zStar = BarrierEquilibriumGap(mat.density*gn, contact.barrier.stiffness, dhat);
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.sizeU = spec.sizeV = size;
	spec.verticesU = spec.verticesV = static_cast<std::uint32_t>(settings.resolution + 1);
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{-0.5*size, xi + zStar, -0.5*size}};
	ModelConfig config = ContactConfig(settings, contact);
	config.gravity = {{gt, -gn, 0}};
	config.damping = false;
	Collider plane;
	plane.type = Collider::Type::Plane; plane.a = {{0, 0, 0}}; plane.b = {{0, 1, 0}};
	config.colliders.push_back(plane);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-6*Gravity;
	const double T = 0.5, dt = 1.0/240;
	const int steps = static_cast<int>(std::lround(T/dt));
	CaseResult r;
	r.resolution = settings.resolution;
	auto meanVelocity = [&]() {
		double s = 0, m = 0;
		for (std::uint32_t k = 0; k < system.Vertices(); ++k) { s += system.GetSheet().mass[k]*system.v[3*static_cast<std::size_t>(k)]; m += system.GetSheet().mass[k]; }
		return s/m;
	};
	double vHalf = 0, smallest = std::numeric_limits<double>::infinity();
	for (int k = 0; k < steps; ++k) {
		if (!system.Step(dt, settings.scheme, newton, r.stats, error)) return Failed(error);
		smallest = std::min(smallest, system.ContactGap(system.x, system.time));
		if (k + 1 == steps/2) vHalf = meanVelocity();
	}
	const double vEnd = meanVelocity();
	const double reference = gt - mu*gn;
	r.ok = true;
	if (reference <= 0) {
		// Sticking under smoothed friction is a steady creep at the speed where
		// f1(v) = tan(angle)/mu: v = eps (1 - sqrt(1 - tan(angle)/mu)).
		const double eps = contact.barrier.frictionVelocity, ratio = std::tan(angle)/mu;
		const double creep = eps*(1 - std::sqrt(1 - ratio));
		r.measured = vEnd;
		r.reference = creep;
		r.error = std::fabs(vEnd - creep)/eps;
		r.detail = Format("sticks: creeps at %.6e m/s (smoothed law %.6e m/s, slip speed %.1e m/s); tan(angle)/mu = %.3f",
			vEnd, creep, eps, ratio) + Format("; smallest gap %.2e m", smallest);
	} else {
		const double a = (vEnd - vHalf)/(T - dt*(steps/2));
		r.measured = a;
		r.reference = reference;
		r.error = std::fabs(a - reference)/reference;
		r.detail = Format("slides: acceleration %.6f m/s^2 (g(sin - mu cos) = %.6f); smallest gap %.2e m",
			a, reference, smallest);
	}
	if (!(smallest > 0)) r.error = std::max(r.error, 1.0);
	return r;
}

namespace {
// A cube of side `side` centred at c, rotated by `ax` about x then `az` about z.
MeshCollider CubeMesh(const V3& c, double side, double ax, double az)
{
	MeshCollider m;
	const double h = 0.5*side;
	for (int k = 0; k < 8; ++k) {
		V3 p{{(k & 1) ? h : -h, (k & 2) ? h : -h, (k & 4) ? h : -h}};
		const V3 p1{{p[0], std::cos(ax)*p[1] - std::sin(ax)*p[2], std::sin(ax)*p[1] + std::cos(ax)*p[2]}};
		const V3 p2{{std::cos(az)*p1[0] - std::sin(az)*p1[1], std::sin(az)*p1[0] + std::cos(az)*p1[1], p1[2]}};
		for (int d = 0; d < 3; ++d) m.vertices.push_back(c[d] + p2[d]);
	}
	const std::uint32_t faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
		{2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
	for (const auto& f : faces) m.triangles.insert(m.triangles.end(), {f[0], f[1], f[2]});
	return m;
}
}

CaseResult RunContactMesh(const CaseSettings& settings, const ContactSettings& contact, bool rotating, double duration)
{
	const double size = 0.5, xi = contact.thickness;
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.sizeU = spec.sizeV = size;
	spec.verticesU = spec.verticesV = static_cast<std::uint32_t>(settings.resolution + 1);
	// Tilted by 0.2 rad about z, its centre 0.08 m off the cube's axis, 0.1 m above the top corner.
	const double tilt = 0.2;
	spec.axisU = {{std::cos(tilt), std::sin(tilt), 0}}; spec.axisV = {{0, 0, 1}};
	ModelConfig config = ContactConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	MeshCollider cube = CubeMesh({{0, 0, 0}}, 0.2, 0.6154797, 0.7853982); // a corner pointing up
	double top = -1;
	for (std::size_t k = 1; k < cube.vertices.size(); k += 3) top = std::max(top, cube.vertices[k]);
	spec.origin = {{0.08 - 0.5*size*spec.axisU[0], top + 0.1 + xi - 0.5*size*spec.axisU[1], -0.5*size}};
	if (rotating) {
		cube.motion.type = Motion::Type::Rotate;
		cube.motion.center = {{0, 0, 0}}; cube.motion.axis = {{0, 1, 0}}; cube.motion.rate = 3.0;
	}
	config.meshColliders.push_back(cube);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-3;
	const double dt = 1.0/240;
	const int steps = static_cast<int>(std::lround(duration/dt));
	CaseResult r;
	r.resolution = settings.resolution;
	int violations = 0, contactSteps = 0;
	double smallest = std::numeric_limits<double>::infinity();
	std::string first;
	for (int k = 0; k < steps; ++k) {
		const std::vector<double> before = system.x;
		const double t0 = system.time;
		const int halvings = r.stats.halvings;
		if (!system.Step(dt, settings.scheme, newton, r.stats, error)) return Failed(error);
		std::string detail;
		// The cloth's straight path while the cube moves on its chord over the step.
		if (!system.PathIntersectionFree(before, system.x, t0, system.time, detail)) { ++violations; if (first.empty()) first = detail; }
		const double gap = system.ContactGap(system.x, system.time);
		if (!(gap > 0)) { ++violations; if (first.empty()) first = "gap " + std::to_string(gap); }
		if (std::isfinite(gap)) ++contactSteps;
		smallest = std::min(smallest, gap);
		(void)halvings;
	}
	r.ok = true;
	r.error = violations;
	r.measured = smallest;
	r.secondary = r.stats.ccdLimited;
	r.detail = Format("%.0f violations in %.0f steps (%.0f with pairs in reach); smallest gap %.3e m; ", violations, steps,
		contactSteps, smallest) + Format("CCD shortened %.0f line searches; %.0f halvings", r.stats.ccdLimited, r.stats.halvings) +
		(first.empty() ? std::string() : "; first: " + first);
	return r;
}

CaseResult RunContactBounce(const CaseSettings& settings, const ContactSettings& contact)
{
	const double size = 0.2, xi = contact.thickness, drop = 0.05;
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.sizeU = spec.sizeV = size;
	spec.verticesU = spec.verticesV = static_cast<std::uint32_t>(settings.resolution + 1);
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{-0.5*size, xi + contact.barrier.activation + drop, -0.5*size}};
	ModelConfig config = ContactConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	config.damping = false;
	config.barrier.friction = 0;
	Collider plane;
	plane.type = Collider::Type::Plane; plane.a = {{0, 0, 0}}; plane.b = {{0, 1, 0}};
	config.colliders.push_back(plane);
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-6*Gravity;
	auto energy = [&]() {
		return system.KineticEnergy(system.v) + system.GravityPotential(system.x) + system.ElasticEnergy(system.x) +
			system.ContactEnergy(system.x, system.time);
	};
	const double e0 = energy();
	double mass = 0;
	for (double m : system.GetSheet().mass) mass += m;
	const double scale = mass*Gravity*drop;
	const double dt = 1.0/240, T = 0.4;
	const int steps = static_cast<int>(std::lround(T/dt));
	double worst = -std::numeric_limits<double>::infinity(), smallest = std::numeric_limits<double>::infinity(), peak = 0;
	bool bounced = false;
	double lowest = std::numeric_limits<double>::infinity();
	CaseResult r;
	r.resolution = settings.resolution;
	for (int k = 0; k < steps; ++k) {
		if (!system.Step(dt, settings.scheme, newton, r.stats, error)) return Failed(error);
		worst = std::max(worst, (energy() - e0)/scale);
		const double gap = system.ContactGap(system.x, system.time);
		smallest = std::min(smallest, gap);
		double y = 0;
		for (std::uint32_t q2 = 0; q2 < system.Vertices(); ++q2) y += system.x[3*static_cast<std::size_t>(q2) + 1]/system.Vertices();
		lowest = std::min(lowest, y);
		if (y > lowest + 1e-4) bounced = true;
		if (bounced) peak = std::max(peak, y);
	}
	r.ok = true;
	r.error = std::max(0.0, worst);
	if (!(smallest > 0)) r.error = std::max(r.error, 1.0);
	r.measured = worst;
	r.secondary = peak;
	r.detail = Format("largest energy rise / drop energy %.3e (final change %.3e); smallest gap %.3e m; bounce peak %.4f m",
		worst, (energy() - e0)/scale, smallest, peak) + Format(" (dropped from %.4f m)", spec.origin[1]);
	return r;
}

// ---------------------------------------------------------------- self-collision cases

namespace {
// Two square layers of `cells` x `cells` cells (side `size`) in one sheet: rows
// 0 .. n-1 the lower layer, n .. 2n-1 the upper, no element joining them (the
// rows next to the cut get boundary masses and rod shares).  The upper layer
// sits at `offset` from the lower one (world), the lower at `spec.origin`.
bool BuildLayers(SheetSpec spec, const Material& material, int cells, double size, const V3& offset, Sheet& sheet,
	std::string& error)
{
	const std::uint32_t n = static_cast<std::uint32_t>(cells + 1);
	spec.verticesU = n; spec.verticesV = 2*n;
	spec.sizeU = size;
	spec.sizeV = size*static_cast<double>(2*n - 1)/static_cast<double>(n - 1);
	Sheet s;
	if (!BuildSheet(spec, material, s, error)) return false;
	auto layer = [&](std::uint32_t v) { return v/n >= n ? 1 : 0; }; // vertex v = i + n j
	auto mixed = [&](const std::uint32_t* v, int count) {
		for (int k = 1; k < count; ++k) if (layer(v[k]) != layer(v[0])) return true;
		return false;
	};
	std::vector<Triangle> tris;
	for (const auto& t : s.triangles) if (!mixed(t.v, 3)) tris.push_back(t);
	std::vector<Hinge> hinges;
	for (const auto& h : s.hinges) if (!mixed(h.v, 4)) hinges.push_back(h);
	std::vector<Rod> rods;
	for (auto r : s.rods) {
		if (mixed(r.v, 3)) continue;
		const std::uint32_t j = r.v[1]/n;
		if (r.kind == HingeKind::Warp && (j == n - 1 || j == n)) r.stiffness *= 0.5; // now an edge row
		rods.push_back(r);
	}
	s.triangles.swap(tris); s.hinges.swap(hinges); s.rods.swap(rods);
	for (std::uint32_t i = 0; i < n; ++i)
		for (std::uint32_t j : {n - 1, n}) { const std::uint32_t q = s.Index(i, j); s.mass[q] *= 0.5; s.area[q] *= 0.5; }
	for (std::uint32_t j = n; j < 2*n; ++j) for (std::uint32_t i = 0; i < n; ++i) {
		const std::uint32_t q = s.Index(i, j);
		const double u = static_cast<double>(i)*s.hu, v = static_cast<double>(j - n)*s.hv;
		for (int d = 0; d < 3; ++d) s.initial[3*q + d] = spec.origin[d] + offset[d] + spec.axisU[d]*u + spec.axisV[d]*v;
	}
	sheet = std::move(s);
	return true;
}

ModelConfig SelfConfig(const CaseSettings& settings, const ContactSettings& contact)
{
	ModelConfig config = ContactConfig(settings, contact);
	config.barrier.selfCollision = true;
	return config;
}
}

CaseResult RunSelfStack(const CaseSettings& settings, const ContactSettings& contact)
{
	const Material& mat = settings.material;
	const int cells = settings.resolution;
	const double size = 0.1, dhat = contact.barrier.activation, xi = contact.thickness, h = size/cells;
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{-0.5*size, xi + 0.5*dhat, -0.5*size}};
	ModelConfig config = SelfConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	config.damping = false;
	config.barrier.friction = 0;
	config.barrier.selfFriction = 0;
	Collider plane;
	plane.type = Collider::Type::Plane; plane.a = {{0, 0, 0}}; plane.b = {{0, 1, 0}};
	config.colliders.push_back(plane);
	const double wall = 0.5*size + xi + dhat;
	for (int k = 0; k < 4; ++k) {
		Collider w;
		w.type = Collider::Type::Plane;
		w.a = {{k == 0 ? -wall : k == 1 ? wall : 0, 0, k == 2 ? -wall : k == 3 ? wall : 0}};
		w.b = {{k == 0 ? 1.0 : k == 1 ? -1.0 : 0, 0, k == 2 ? 1.0 : k == 3 ? -1.0 : 0}};
		config.colliders.push_back(w);
	}
	// The layers aligned in plan.
	Sheet sheet;
	std::string error;
	(void)h;
	if (!BuildLayers(spec, mat, cells, size, {{0, xi + 0.5*dhat, 0}}, sheet, error)) return Failed(error);
	ClothSystem system;
	if (!system.Initialize(sheet, config, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-9*Gravity;
	newton.maxIterations = 100;
	CaseResult r;
	r.resolution = cells;
	// Frictionless layers slide freely: the solve keeps the last stage's mass
	// term (each stage converged), and frictionless walls keep the layers over
	// each other (their forces are horizontal: the vertical balance is unaffected).
	if (!system.SolveStatic(newton, r.stats, error, 0.01, 10, false, 100)) return Failed(error);
	const std::uint32_t n = static_cast<std::uint32_t>(cells + 1), count = system.Vertices();
	double upperWeight = 0, weight = 0;
	for (std::uint32_t q = 0; q < count; ++q) {
		weight += sheet.mass[q]*Gravity;
		if (q/n >= n) upperWeight += sheet.mass[q]*Gravity;
	}
	std::vector<double> forces;
	system.ContactForces(system.x, system.time, forces);
	double upperForce = 0, totalForce = 0;
	for (std::uint32_t q = 0; q < count; ++q) {
		totalForce += forces[3*static_cast<std::size_t>(q) + 1];
		if (q/n >= n) upperForce += forces[3*static_cast<std::size_t>(q) + 1];
	}
	// Gap between the layers above the offset: each upper vertex's height above
	// the lower vertex under it, less xi.
	double gapMin = std::numeric_limits<double>::infinity(), gapMax = -gapMin, gapMean = 0;
	for (std::uint32_t q = n*n; q < count; ++q) {
		const double z = system.x[3*static_cast<std::size_t>(q) + 1] - system.x[3*static_cast<std::size_t>(q - n*n) + 1] - xi;
		gapMin = std::min(gapMin, z); gapMax = std::max(gapMax, z); gapMean += z/(n*n);
	}
	const double planeBalance = std::fabs(totalForce - weight)/weight, layerBalance = std::fabs(upperForce - upperWeight)/upperWeight;
	r.ok = true;
	r.error = std::max(planeBalance, layerBalance);
	if (!(gapMin > 0) || !(gapMax < dhat) || !(system.ContactGap(system.x, system.time) > 0)) r.error = 1;
	r.measured = gapMean;
	r.reference = dhat;
	r.secondary = layerBalance;
	r.detail = Format("plane force / both weights - 1 = %.2e; force on the upper layer / its weight - 1 = %.2e; ",
		(totalForce - weight)/weight, (upperForce - upperWeight)/upperWeight) +
		Format("gap between layers above the offset %.4e m (%.4e .. %.4e) for dhat %.1e m", gapMean, gapMin, gapMax, dhat);
	return r;
}

CaseResult RunSelfIncline(const CaseSettings& settings, const ContactSettings& contact, double angle)
{
	const Material& mat = settings.material;
	const int cells = settings.resolution;
	const double size = 0.1, dhat = contact.barrier.activation, xi = contact.thickness, mu = contact.barrier.selfFriction;
	const double h = size/cells, gn = Gravity*std::cos(angle), gt = Gravity*std::sin(angle);
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{-0.5*size, 0, -0.5*size}};
	ModelConfig config = SelfConfig(settings, contact);
	config.gravity = {{gt, -gn, 0}};
	config.damping = false;
	Sheet sheet;
	std::string error;
	// The layers aligned in plan.
	(void)h;
	if (!BuildLayers(spec, mat, cells, size, {{0, xi + 0.5*dhat, 0}}, sheet, error)) return Failed(error);
	const std::uint32_t n = static_cast<std::uint32_t>(cells + 1);
	PinGroup lower;
	for (std::uint32_t q = 0; q < n*n; ++q) lower.vertices.push_back(q);
	config.pins.push_back(lower);
	ClothSystem system;
	if (!system.Initialize(sheet, config, error)) return Failed(error);
	CaseResult r;
	r.resolution = cells;
	// Settle the upper layer on the lower one (static, gravity normal to the layers), then tilt.
	NewtonOptions settle = settings.newton;
	settle.tolerance = 1e-8*Gravity;
	settle.maxIterations = 100;
	{
		// Frictionless walls (horizontal forces only) keep the upper layer over the lower one.
		ModelConfig normal = config;
		normal.gravity = {{0, -gn, 0}};
		const double wall = 0.5*size + xi + dhat;
		for (int k = 0; k < 4; ++k) {
			Collider w;
			w.type = Collider::Type::Plane;
			w.a = {{k == 0 ? -wall : k == 1 ? wall : 0, 0, k == 2 ? -wall : k == 3 ? wall : 0}};
			w.b = {{k == 0 ? 1.0 : k == 1 ? -1.0 : 0, 0, k == 2 ? 1.0 : k == 3 ? -1.0 : 0}};
			normal.colliders.push_back(w);
		}
		ClothSystem rest;
		if (!rest.Initialize(sheet, normal, error)) return Failed(error);
		if (!rest.SolveStatic(settle, r.stats, error, 0.01, 10, false, 100)) return Failed(error);
		system.x = rest.x;
		// m3a.8: the frictionless settle leaves the upper layer's in-plane rigid motion neutral (only the
		// walls, 12.5 um away, bound it), so round-off decided which corner it drifted into, and the
		// measured creep or acceleration with it (stick 0.68e-2 .. 1.78e-2 under 1-ulp perturbations of
		// the old solver; docs/SIMULATION_PERFORMANCE.md section 13.7).  The settle supplies the normal
		// (contact) equilibrium only: the upper layer starts aligned in plan, as the case states.
		for (std::uint32_t q = n*n; q < system.Vertices(); ++q)
			for (int d : {0, 2}) system.x[3*static_cast<std::size_t>(q) + d] = sheet.initial[3*static_cast<std::size_t>(q) + d];
		system.v.assign(system.x.size(), 0.0);
	}
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-6*Gravity;
	// Short enough that the sliding layer stays over the lower one (0.1 m wide; it
	// would otherwise overhang, droop and lose support).
	const double T = 0.1, dt = 1.0/240;
	const int steps = static_cast<int>(std::lround(T/dt));
	auto meanVelocity = [&]() {
		double s = 0, m = 0;
		for (std::uint32_t k = n*n; k < system.Vertices(); ++k) { s += sheet.mass[k]*system.v[3*static_cast<std::size_t>(k)]; m += sheet.mass[k]; }
		return s/m;
	};
	double vHalf = 0, smallest = std::numeric_limits<double>::infinity();
	for (int k = 0; k < steps; ++k) {
		if (!system.Step(dt, settings.scheme, newton, r.stats, error)) return Failed(error);
		smallest = std::min(smallest, system.ContactGap(system.x, system.time));
		if (k + 1 == steps/2) vHalf = meanVelocity();
	}
	const double vEnd = meanVelocity();
	const double reference = gt - mu*gn;
	r.ok = true;
	if (reference <= 0) {
		const double eps = contact.barrier.frictionVelocity, ratio = std::tan(angle)/mu;
		const double creep = eps*(1 - std::sqrt(1 - ratio));
		r.measured = vEnd;
		r.reference = creep;
		r.error = std::fabs(vEnd - creep)/eps;
		r.detail = Format("upper layer sticks: creeps at %.6e m/s (smoothed law %.6e m/s, slip speed %.1e m/s); tan(angle)/mu = %.3f",
			vEnd, creep, eps, ratio) + Format("; smallest gap %.2e m", smallest);
	} else {
		const double a = (vEnd - vHalf)/(T - dt*(steps/2));
		r.measured = a;
		r.reference = reference;
		r.error = std::fabs(a - reference)/reference;
		r.detail = Format("upper layer slides: acceleration %.6f m/s^2 (g(sin - mu cos) = %.6f); smallest gap %.2e m",
			a, reference, smallest);
	}
	if (!(smallest > 0)) r.error = std::max(r.error, 1.0);
	return r;
}

CaseResult RunSelfHairpin(const CaseSettings& settings, const ContactSettings& contact)
{
	// A strip folded over itself on a frictionless plane (static): the lower
	// half on the plane, a loop, the upper half on the lower one.  Both ends are
	// held, the upper one above the lower at the offset (plus half the barrier's
	// reach): on frictionless supports the fold could otherwise roll along the
	// strip at no cost (a free mode), which the held ends remove.
	const Material& mat = settings.material;
	const double B = mat.bendWarp, q = mat.density*Gravity;
	if (!(B > 0)) return Failed("the hairpin needs bending rigidity");
	const double lb = std::cbrt(B/q), L = 16*lb, width = 0.5*lb, xi = contact.thickness, dhat = contact.barrier.activation;
	const int segments = 2*((settings.resolution + 1)/2);
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	spec.sizeU = L; spec.sizeV = width;
	spec.verticesU = static_cast<std::uint32_t>(segments + 1); spec.verticesV = 3;
	spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
	spec.origin = {{0, xi + 0.5*dhat, 0}};
	ModelConfig config = SelfConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	config.damping = false;
	config.barrier.friction = 0;
	config.barrier.selfFriction = 0;
	Collider plane;
	plane.type = Collider::Type::Plane; plane.a = {{0, 0, 0}}; plane.b = {{0, 1, 0}};
	config.colliders.push_back(plane);
	const std::uint32_t nu = spec.verticesU;
	PinGroup ends;
	for (std::uint32_t j = 0; j < 3; ++j) { ends.vertices.push_back(nu*j); ends.vertices.push_back(nu - 1 + nu*j); }
	config.pins.push_back(ends);
	Sheet sheet;
	std::string error;
	if (!BuildSheet(spec, mat, sheet, error)) return Failed(error);
	// Initial shape: flat for a third, a semicircle of radius r0, then straight
	// back down to the upper end held at x = 0, xi + dhat/2 above the lower end.
	const double r0 = 1.5*lb, arc = Pi*r0, flat = L/3, y0 = xi + 0.5*dhat, yTop = y0 + xi + 0.5*dhat;
	const double back = L - flat - arc;
	for (std::uint32_t k = 0; k < sheet.Vertices(); ++k) {
		const double s = sheet.materialU[k];
		double x, y;
		if (s <= flat) { x = s; y = y0; }
		else if (s <= flat + arc) { const double a = (s - flat)/r0; x = flat + r0*std::sin(a); y = y0 + r0*(1 - std::cos(a)); }
		else {
			const double f = (s - flat - arc)/back; // 0 at the loop's top, 1 at the upper end
			x = flat*(1 - f); y = (y0 + 2*r0)*(1 - f) + yTop*f;
		}
		sheet.initial[3*k] = x; sheet.initial[3*k + 1] = y;
	}
	ClothSystem system;
	if (!system.Initialize(sheet, config, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-8*Gravity;
	newton.maxIterations = 200;
	CaseResult r;
	r.resolution = segments;
	if (!system.SolveStatic(newton, r.stats, error, 0.01, 12, true, 100)) return Failed(error);
	// Loop height (the middle row's highest point above the plane) and the
	// fold's reach (the loop's outermost x from the ends).
	double top = 0, reachX = -std::numeric_limits<double>::infinity();
	for (std::uint32_t i = 0; i < nu; ++i) {
		const std::size_t k = 3*static_cast<std::size_t>(i + nu);
		top = std::max(top, system.x[k + 1]);
		reachX = std::max(reachX, system.x[k]);
	}
	r.ok = system.ContactGap(system.x, system.time) > 0;
	if (!r.ok) r.failure = "the hairpin ends inside an offset";
	r.measured = top;
	r.secondary = reachX;
	r.reference = lb;
	r.detail = Format("loop height %.6e m, fold reach %.6e m (bending length %.4e m); smallest gap %.3e m", top, reachX, lb,
		system.ContactGap(system.x, system.time));
	return r;
}

CaseResult RunSelfFold(const CaseSettings& settings, const ContactSettings& contact, int scenario, double duration)
{
	// Exact no-intersection check of every accepted step, cloth against the
	// colliders and against itself.  Scenario 0: a sheet dropped corner-first
	// onto a sphere so that it folds over it; 1: a strip on a plane pushed into a
	// fold by pins at its two ends moving toward each other.
	const double xi = contact.thickness, dhat = contact.barrier.activation;
	SheetSpec spec;
	spec.triangulation = settings.triangulation; spec.bending = settings.bending;
	ModelConfig config = SelfConfig(settings, contact);
	config.gravity = {{0, -Gravity, 0}};
	const std::uint32_t n = static_cast<std::uint32_t>(settings.resolution + 1);
	if (scenario == 0) {
		spec.sizeU = spec.sizeV = 0.6;
		spec.verticesU = spec.verticesV = n;
		const double t = 0.35, lu = std::sqrt(1 + t*t);
		const V3 u{{1/lu, t/lu, 0}};
		V3 w{{0, t, 1}};
		const double d = w[0]*u[0] + w[1]*u[1] + w[2]*u[2];
		w = {{w[0] - d*u[0], w[1] - d*u[1], w[2] - d*u[2]}};
		const double lw = std::sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
		spec.axisU = u; spec.axisV = {{w[0]/lw, w[1]/lw, w[2]/lw}};
		spec.origin = {{-0.1, 0.85, -0.1}};
		Collider sphere;
		sphere.type = Collider::Type::Sphere; sphere.a = {{0, 0.5, 0}}; sphere.radius = 0.3;
		config.colliders.push_back(sphere);
	} else {
		spec.sizeU = 0.3; spec.sizeV = 0.06;
		spec.verticesU = n; spec.verticesV = std::max<std::uint32_t>(3, n/5 + 1);
		spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, 1}};
		spec.origin = {{-0.15, xi + 0.5*dhat, -0.03}};
		Collider plane;
		plane.type = Collider::Type::Plane; plane.a = {{0, 0, 0}}; plane.b = {{0, 1, 0}};
		config.colliders.push_back(plane);
		PinGroup left, right;
		for (std::uint32_t j = 0; j < spec.verticesV; ++j) { left.vertices.push_back(n*j); right.vertices.push_back(n - 1 + n*j); }
		left.motion.type = right.motion.type = Motion::Type::Translate;
		// The left end is carried up and over toward the right end (folding the
		// strip onto itself, like folding a towel); the right end moves in a little.
		left.motion.vector = {{0.22, 0.05, 0}}; right.motion.vector = {{-0.03, 0, 0}};
		config.pins.push_back(left); config.pins.push_back(right);
	}
	ClothSystem system;
	std::string error;
	if (!Build(spec, config, system, error)) return Failed(error);
	NewtonOptions newton = settings.newton;
	newton.tolerance = 1e-3;
	const double dt = 1.0/240;
	const int steps = static_cast<int>(std::lround(duration/dt));
	CaseResult r;
	r.resolution = settings.resolution;
	int violations = 0, selfSteps = 0;
	double smallest = std::numeric_limits<double>::infinity();
	std::string first;
	for (int k = 0; k < steps; ++k) {
		const std::vector<double> before = system.x;
		const double t0 = system.time;
		if (!system.Step(dt, settings.scheme, newton, r.stats, error)) return Failed(error);
		std::string detail;
		if (!system.PathIntersectionFree(before, system.x, t0, system.time, detail)) { ++violations; if (first.empty()) first = detail; }
		const double gap = system.ContactGap(system.x, system.time);
		if (!(gap > 0)) { ++violations; if (first.empty()) first = "gap " + std::to_string(gap); }
		if (system.SelfContactPairs(system.x) > 0) ++selfSteps;
		smallest = std::min(smallest, gap);
	}
	r.ok = true;
	r.error = violations;
	r.measured = smallest;
	r.secondary = selfSteps;
	r.detail = Format("%.0f violations in %.0f steps (%.0f with cloth-cloth pairs in reach); smallest gap %.3e m; ", violations, steps,
		selfSteps, smallest) + Format("%.0f Newton iterations, %.0f halvings", r.stats.newtonIterations, r.stats.halvings) +
		(first.empty() ? std::string() : "; first: " + first);
	return r;
}

} } }
