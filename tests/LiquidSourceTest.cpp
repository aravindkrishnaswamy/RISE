// liquid.mac: inflow sources and drains (simulation M9, docs/SIMULATION_M9_RESULTS.md; preview).
//
//   A  volume accounting: a nozzle (cylinder, 1 m/s) over a pool, with a drain; the volume each adds or
//      removes, measured as the volume correction measures the liquid, against the nominal flux
//      (exit area x speed x time, plus the region's first fill), and the liquid's volume against the
//      correction's target (initial + added - drained); a ramped schedule; a box source
//   B  a downward jet: its front against free fall from the nozzle at the exit speed, and the exit speed
//      (the face velocity just below the nozzle); a horizontal jet's centreline against the parabola
//   C  rain (drops): the drops released against the count the window and period give, every drop's
//      volume accounted for
//   D  with a rigid body (a floating box in a basin filled by a source rises with the level) and with a
//      held sheet (rain on a prescribed sheet: nothing below it)
//   E  the solver: determinism across thread counts (1, 3), a checkpoint in the middle of the window
//      restored equals the uninterrupted run, effective parameters without sources are unchanged
//      (an empty `sources` list is the absent one; the configuration's key does not move), refusals,
//      the chunk syntax
// Measurement knobs (not used by the suite): LST_CELLS=<n> runs A at n cells, LST_SCALE=<k> runs B's downward jet
// at k times its resolution; `LiquidSourceTest <A|B|C|D|E>` runs one case.
#include "../src/Library/Simulation/Liquid/LiquidModel.h"
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidParameterSchema.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationMesh.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace {
using namespace RISE::Simulation;
using namespace RISE::Simulation::Liquid;
using V = RISE::RISECBOR64::Value;
int failures = 0;
constexpr double Pi = 3.14159265358979323846;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

Shape Fill(double height)
{
	Shape s;
	s.type = Shape::Type::Plane;
	s.point = {{0, height, 0}};
	s.axis = {{0, 1, 0}};
	return s;
}

LiquidSetup Tank(int nx, int ny, int nz, double h, const V3& origin)
{
	LiquidSetup s;
	s.grid.n = {{nx, ny, nz}};
	s.grid.h = h;
	s.grid.origin = origin;
	return s;
}

LiquidSource Nozzle(const char* name, const V3& centre, const V3& axis, double r, double length, double speed, double start, double end)
{
	LiquidSource s;
	s.name = name;
	s.type = LiquidSource::Type::Cylinder;
	s.center = centre;
	s.axis = axis;
	s.direction = axis;
	s.radius = r;
	s.height = length;
	s.speed = speed;
	s.start = start;
	s.end = end;
	return s;
}

SolverOptions Options()
{
	SolverOptions o;
	return o;
}

// ---------------------------------------------------------------- A

void Accounting()
{
	std::printf("A: volume accounting\n");
	std::string error;
	// 0.3 m box, 24 cells (1.25 cm); a 2 cm layer; a nozzle of radius 2.5 cm (2 cells), 3 cm long, 1 m/s
	// down for 0.3 s; a drain in a corner from 0.2 s.
	const int cells = std::getenv("LST_CELLS") ? std::atoi(std::getenv("LST_CELLS")) : 24;
	const double h = 0.3/cells;
	for (int variant = 0; variant < 3; ++variant) {
		LiquidSetup s = Tank(cells, cells, cells, h, {{-0.15, 0, -0.15}});
		s.liquid.push_back(Fill(0.02));
		LiquidSource n = Nozzle("nozzle", {{0, 0.25, 0}}, {{0, -1, 0}}, 0.025, 0.03, 1.0, 0.0, 0.3);
		if (variant == 1) n.schedule = {0.0, 0.0, 0.1, 1.0, 0.2, 1.0, 0.3, 0.5};   // ramp up, hold, ease
		if (variant == 2) {
			// A box source (5 x 3 x 5 cm, turned 0.3 rad about y), 0.6 m/s down, from 0.05 s.
			n = LiquidSource();
			n.name = "box";
			n.type = LiquidSource::Type::Box;
			n.center = {{0.0, 0.24, 0.0}};
			n.half = {{0.025, 0.015, 0.025}};
			const double c = std::cos(0.3), sn = std::sin(0.3);
			n.R = {{c, 0, sn, 0, 1, 0, -sn, 0, c}};
			n.direction = {{0, -1, 0}};
			n.speed = 0.6;
			n.start = 0.05;
			n.end = 0.3;
		}
		s.sources.push_back(n);
		LiquidSource d;
		d.name = "drain";
		d.kind = LiquidSource::Kind::Drain;
		d.type = LiquidSource::Type::Box;
		d.center = {{0.12, 0.01, 0.12}};
		d.half = {{0.03, 0.03, 0.03}};
		d.start = 0.2;
		s.sources.push_back(d);
		SolverOptions o = Options();
		if (variant > 0) o.particles = false;   // time: variant 0 has the marker particles
		LiquidModel m;
		if (!m.Initialize(s, o, 8, error)) { Check(false, "accounting model", error); return; }
		const double V0 = m.Volume(), T = 0.4;
		const double t0 = Seconds();
		double worst = 0, fill = -1, tFill = 0;
		if (!m.Run(T, 1.0/60, [&](double t, const StepStats&) {
			if (fill < 0 && m.sourceVolume[0] > 0) { fill = m.sourceVolume[0]; tFill = t; }
			const double expect = V0 + m.sourceVolume[0] - m.sourceVolume[1];
			worst = std::max(worst, std::fabs(m.TargetVolume() - expect)/expect);
		}, error)) { Check(false, "accounting run", error); return; }
		const LiquidSource& src = m.Setup().sources[0];
		const double added = m.sourceVolume[0], flux = src.Nominal(0, T) - src.Nominal(0, tFill), region = src.RegionVolume();
		const double drained = m.sourceVolume[1], measured = m.sourceMeasured[0];
		const double vol = m.Volume(), target = m.TargetVolume();
		std::printf("  %s: added %.4f L = first fill %.4f L (the region %.4f L) + nominal flux %.4f L (exit area x speed x the factor "
			"from %.4f s); the level set's refills %.4f L (%+.1f%% of the added); drained %.4f L; liquid %.4f L, target %.4f L (initial "
			"%.4f + added - drained: off by %.1e); %d steps, %.1f s\n",
			variant == 0 ? "nozzle" : variant == 1 ? "nozzle, ramped" : "box, turned", 1000*added, 1000*fill, 1000*region, 1000*flux, tFill,
			1000*measured, 100*(measured/added - 1), 1000*drained, 1000*vol, 1000*target, 1000*V0, worst, int(m.steps), Seconds() - t0);
		Check(std::fabs(added - (fill + flux)) < 1e-9*added, "the volume added is the first fill plus the nominal flux");
		Check(std::fabs(fill/region - 1) < 0.15, "the first fill is the region's volume (15%: the smoothed measure of a region of a few cells)");
		Check(worst < 1e-12, "the correction's target is the initial volume plus every source's less every drain's");
		Check(drained > 0, "the drain removes liquid");
		Check(std::fabs(vol - target)/target < 0.02, "the liquid's volume follows the target (2%)");
	}
}

// ---------------------------------------------------------------- B

// The lowest liquid point on the column through (x, z) below y0: the zero crossing below the last liquid cell
// of the run of liquid cells that starts at the nozzle.
double Front(const LiquidModel& m, double x, double z, double y0)
{
	const Grid& g = m.GridSpec();
	const int i = int(std::floor((x - g.origin[0])/g.h)), k = int(std::floor((z - g.origin[2])/g.h));
	int j = int(std::floor((y0 - g.origin[1])/g.h));
	while (j > 0 && m.phi[g.C(i, j - 1, k)] < 0) --j;
	if (j == 0) return g.origin[1];
	const double a = m.phi[g.C(i, j, k)], b = m.phi[g.C(i, j - 1, k)];
	return g.CellCentre(i, j, k)[1] - g.h*a/(a - b);
}

// The liquid's cross-section area in the horizontal slice of cells around height y (sharp fill clamp(1/2 - phi/h)).
double SliceArea(const LiquidModel& m, double y)
{
	const Grid& g = m.GridSpec();
	const int j = int(std::floor((y - g.origin[1])/g.h));
	double a = 0;
	for (int k = 0; k < g.n[2]; ++k) for (int i = 0; i < g.n[0]; ++i)
		a += std::min(1.0, std::max(0.0, 0.5 - m.phi[g.C(i, j, k)]/g.h))*g.h*g.h;
	return a;
}

void Jets()
{
	std::printf("B: jets\n");
	std::string error;
	const double g = 9.81;
	{
		// A downward jet: 0.2 x 0.5 x 0.2 m at 40 cells along y (1.25 cm), nozzle radius 3 cm (2.4 cells), 6 cm long,
		// its exit at y = 0.43 m, 1 m/s; after 0.16 s the axis's velocity, the jet's radius and its front.
		const int scale = std::getenv("LST_SCALE") ? std::atoi(std::getenv("LST_SCALE")) : 1;
		const double h = 0.0125/scale;
		LiquidSetup s = Tank(16*scale, 40*scale, 16*scale, h, {{-0.1, 0, -0.1}});
		s.sources.push_back(Nozzle("nozzle", {{0, 0.46, 0}}, {{0, -1, 0}}, 0.03, 0.06, 1.0, 0.0, 1.0));
		LiquidModel m;
		if (!m.Initialize(s, Options(), 8, error)) { Check(false, "jet model", error); return; }
		const double exit = 0.43, v0 = 1.0, r0 = 0.03, T = 0.16;
		if (!m.Run(T, 1.0/60, nullptr, error)) { Check(false, "jet run", error); return; }
		const Grid& gr = m.GridSpec();
		const int ia = 8*scale, ka = 8*scale;   // the column beside the axis (its centre 0.9 cm off it at scale 1)
		const double front = Front(m, 0.0, 0.0, exit), fall = exit - v0*T - 0.5*g*T*T;
		std::printf("  downward jet at 1 m/s, t = %.2f s: depth below the exit (m), axis speed (m/s), free fall (m/s), radius (m), "
			"continuity r0 sqrt(v0 / v) (m)\n", T);
		double worstSpeed = 0, worstRadius = 0;
		for (double depth : {0.005, 0.03, 0.06, 0.1, 0.15}) {
			const double y = exit - depth;
			const int j = int(std::lround((y - gr.origin[1])/h));
			const double speed = -m.u.u[1][gr.F(1, ia, j, ka)], ff = std::sqrt(v0*v0 + 2*g*(exit - (gr.origin[1] + j*h)));
			const double radius = std::sqrt(SliceArea(m, y)/Pi), cont = r0*std::sqrt(v0/ff);
			std::printf("    %.3f  %.4f  %.4f  %.4f  %.4f\n", depth, speed, ff, radius, cont);
			worstSpeed = std::max(worstSpeed, std::fabs(speed/ff - 1));
			if (depth >= 0.03) worstRadius = std::max(worstRadius, std::fabs(radius - cont)/h);
		}
		std::printf("    front %.4f m (free fall of the first liquid %.4f m; the tip of a jet thinning to %.1f cells erodes)\n", front, fall,
			r0*std::sqrt(v0/(v0 + g*T))/h);
		Check(worstSpeed < 0.06, "the jet leaves at its speed and falls freely below the nozzle (axis speed within 6% of free fall)");
		Check(worstRadius < 0.5, "the jet thins as continuity demands (radius within half a cell)");
		Check(front < exit - 0.7*(exit - fall), "the jet's front has fallen at least 70% of the free-fall distance");
	}
	{
		// A horizontal jet: 0.5 x 0.3 x 0.12 m at 40 cells along x (1.25 cm), nozzle radius 3.5 cm (2.8 cells) along +x at 0.8 m/s,
		// its exit at x = -0.2 m, y = 0.25 m; at 0.3 s the jet's centre height against the parabola, above the floor's splash.
		const double h = 0.0125;
		LiquidSetup s = Tank(40, 24, 10, h, {{-0.25, 0, -0.06}});
		s.sources.push_back(Nozzle("nozzle", {{-0.225, 0.25, 0}}, {{1, 0, 0}}, 0.035, 0.05, 0.8, 0.0, 1.0));
		LiquidModel m;
		if (!m.Initialize(s, Options(), 8, error)) { Check(false, "horizontal jet model", error); return; }
		if (!m.Run(0.3, 1.0/60, nullptr, error)) { Check(false, "horizontal jet run", error); return; }
		const Grid& gr = m.GridSpec();
		std::printf("  horizontal jet at 0.8 m/s, t = 0.3 s: x (m), centre height (m), parabola (m)\n");
		double worst = 0;
		for (double x : {-0.17, -0.14, -0.11, -0.08}) {
			const int i = int(std::floor((x - gr.origin[0])/gr.h)), k = 4;
			double sum = 0, w = 0;
			for (int j = 0; j < gr.n[1]; ++j) {
				const double y = gr.CellCentre(i, j, k)[1];
				if (y < 0.05) continue;   // the floor's splash
				const double f = std::min(1.0, std::max(0.0, 0.5 - m.phi[gr.C(i, j, k)]/gr.h));
				sum += f*y;
				w += f;
			}
			const double xc = gr.CellCentre(i, 0, k)[0], expect = 0.25 - 0.5*g*std::pow((xc + 0.2)/0.8, 2);
			const double y = w > 0 ? sum/w : 0;
			std::printf("    %.4f  %.4f  %.4f  (%+.2f cells)\n", xc, y, expect, (y - expect)/h);
			worst = std::max(worst, std::fabs(y - expect)/h);
		}
		Check(worst < 1.5, "the horizontal jet follows the free-fall parabola (1.5 cells)");
	}
}

// ---------------------------------------------------------------- C

void Rain()
{
	std::printf("C: rain\n");
	std::string error;
	const double h = 0.3/24;
	LiquidSetup s = Tank(24, 24, 24, h, {{-0.15, 0, -0.15}});
	s.liquid.push_back(Fill(0.02));
	LiquidSource r;
	r.name = "rain";
	r.type = LiquidSource::Type::Box;
	r.center = {{0, 0.27, 0}};
	r.half = {{0.12, 0.02, 0.12}};
	r.direction = {{0, -1, 0}};
	r.speed = 1.5;
	r.dropsX = r.dropsZ = 4;
	r.dropRadius = 0.025;   // 2 cells
	r.dropPeriod = 0.1;
	r.end = 0.3;
	s.sources.push_back(r);
	SolverOptions o = Options();
	o.particles = false;
	LiquidModel m;
	if (!m.Initialize(s, o, 8, error)) { Check(false, "rain model", error); return; }
	const double V0 = m.Volume();
	std::size_t released = 0;
	double first = 1e300;
	if (!m.Run(0.4, 1.0/60, [&](double, const StepStats&) {
		released += m.stepDrops_.size();
		for (const auto& d : m.stepDrops_) first = std::min(first, d.time);
	}, error)) { Check(false, "rain run", error); return; }
	std::vector<std::pair<V3, double>> all;
	m.Setup().sources[0].DropsIn(0, 0, 1, all);
	const double drop = 4.0/3.0*Pi*r.dropRadius*r.dropRadius*r.dropRadius;
	std::printf("  %zu drops released (the window gives %zu: 16 columns x 3 periods), first at %.4f s; added %.4f L (the drops' volume "
		"%.4f L), the level set's fills %.4f L (%+.1f%%: drops of 2 cells); liquid %.4f L, target %.4f L (initial %.4f)\n", released,
		all.size(), first, 1000*m.sourceVolume[0], 1000*released*drop, 1000*m.sourceMeasured[0], 100*(m.sourceMeasured[0]/(released*drop) - 1),
		1000*m.Volume(), 1000*m.TargetVolume(), 1000*V0);
	Check(released == 48 && all.size() == 48, "every column releases one drop per period within the window");
	Check(std::fabs(m.sourceVolume[0] - released*drop) < 1e-12, "the target gains each drop's volume");
	Check(std::fabs(m.sourceMeasured[0]/(released*drop) - 1) < 0.3, "each drop's fill is about its volume (30%: a drop of 2 cells)");
	Check(std::fabs(m.TargetVolume() - V0 - m.sourceVolume[0]) < 1e-12, "the drops' volume is in the correction's target");
}

// ---------------------------------------------------------------- D

// A flat sheet through `centre` spanned by u and w (half sizes su, sw), nu x nw vertices, implicitly coupled with a lumped
// mass sigma (kg/m^2), every vertex held (LiquidSheetViscosityTest's MakeSheet).
LiquidSheet HeldSheet(const V3& centre, const V3& u, const V3& w, double su, double sw, int nu, int nw, double sigma)
{
	LiquidSheet sh;
	sh.name = "sheet";
	sh.motion = LiquidSheet::Motion::Port;
	sh.implicit = true;
	sh.topology = "test";
	for (int j = 0; j < nw; ++j) for (int i = 0; i < nu; ++i) {
		const double a = -su + 2*su*i/(nu - 1), b = -sw + 2*sw*j/(nw - 1);
		for (int k = 0; k < 3; ++k) {
			sh.x.push_back(centre[std::size_t(k)] + a*u[std::size_t(k)] + b*w[std::size_t(k)]);
			sh.v.push_back(0.0);
		}
	}
	for (int j = 0; j + 1 < nw; ++j) for (int i = 0; i + 1 < nu; ++i) {
		const std::uint32_t p = std::uint32_t(i + nu*j), q = p + 1, r = p + std::uint32_t(nu) + 1, t = p + std::uint32_t(nu);
		for (std::uint32_t k : {p, q, r, p, r, t}) sh.triangles.push_back(k);
	}
	const std::size_t n = sh.x.size()/3;
	sh.dyn.mass.assign(n, sigma*4*su*sw/double(n));
	sh.dyn.fixed.assign(n, 1);
	sh.dyn.force.assign(3*n, 0.0);
	sh.dyn.x0 = sh.x;
	sh.dyn.xe = sh.x;
	return sh;
}

void WithBodyAndSheet()
{
	std::printf("D: with a body and a sheet\n");
	std::string error;
	{
		// A light box (500 kg/m^3, 7.5 cm) floating at rest in 7.5 cm of water (24 cells, 1.25 cm); a nozzle (radius 3 cm)
		// fills the basin at 1.7 L/s for 0.6 s: the box rises with the level.
		const double h = 0.3/24;
		LiquidSetup s = Tank(24, 24, 24, h, {{-0.15, 0, -0.15}});
		s.liquid.push_back(Fill(0.075));
		LiquidBody b;
		b.name = "float";
		b.shape.type = BodyShape::Type::Box;
		b.shape.size = {{0.075, 0.075, 0.075}};
		b.motion = BodyMotion::Dynamic;
		const double mass = 500*b.shape.Volume();
		b.state.mass = mass;
		b.state.moments = b.shape.Moments(mass);
		b.state.x = {{0.06, 0.075, 0.0}};   // its draft at rest: half its height
		b.state.q = {{1, 0, 0, 0}};
		b.free = {{false, true, false, false}};
		b.x0 = b.state.x;
		b.q0 = b.state.q;
		b.externalForce = {{0, -9.81*mass, 0}};
		s.bodies.push_back(b);
		s.sources.push_back(Nozzle("nozzle", {{-0.08, 0.25, -0.08}}, {{0, -1, 0}}, 0.03, 0.04, 0.6, 0.1, 0.7));
		SolverOptions o = Options();
		o.particles = false;
		LiquidModel m;
		if (!m.Initialize(s, o, 8, error)) { Check(false, "float model", error); return; }
		if (!m.Run(0.1, 1.0/60, nullptr, error)) { Check(false, "float settle", error); return; }
		const double y0 = m.bodies[0].state.x[1];
		if (!m.Run(0.8, 1.0/60, nullptr, error)) { Check(false, "float run", error); return; }
		const double y1 = m.bodies[0].state.x[1];
		const double rise = m.sourceVolume[0]/(0.3*0.3 - 0.075*0.075);
		std::printf("  the float's centre %.4f m -> %.4f m (%+.1f mm); the level rises by the added %.3f L over the open basin: %.1f mm\n",
			y0, y1, 1000*(y1 - y0), 1000*m.sourceVolume[0], 1000*rise);
		Check(std::fabs((y1 - y0)/rise - 1) < 0.3, "the float rises with the level the source adds (30%)");
	}
	{
		// Rain on a held, implicitly coupled sheet across the box at 0.1234 m: nothing passes it.
		const double h = 0.3/32;
		LiquidSetup s = Tank(32, 32, 32, h, {{-0.15, 0, -0.15}});
		s.sheets.push_back(HeldSheet({{0, 0.1234, 0}}, {{1, 0, 0}}, {{0, 0, 1}}, 0.16, 0.16, 17, 17, 0.5));
		LiquidSource r;
		r.name = "rain";
		r.type = LiquidSource::Type::Box;
		r.center = {{0, 0.25, 0}};
		r.half = {{0.12, 0.02, 0.12}};
		r.speed = 1.0;
		r.dropsX = r.dropsZ = 4;
		r.dropRadius = 0.014;
		r.dropPeriod = 0.08;
		r.end = 0.25;
		s.sources.push_back(r);
		SolverOptions o = Options();
		o.particles = false;
		LiquidModel m;
		if (!m.Initialize(s, o, 8, error)) { Check(false, "awning model", error); return; }
		m.ResetSheetDynamics();
		if (!m.Run(0.45, 1.0/60, nullptr, error)) { Check(false, "awning run", error); return; }
		const Grid& g = m.GridSpec();
		double below = 0, above = 0;
		for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
			const double f = std::min(1.0, std::max(0.0, 0.5 - m.phi[g.C(i, j, k)]/g.h))*g.h*g.h*g.h;
			(g.CellCentre(i, j, k)[1] < 0.1234 - g.h ? below : above) += f;
		}
		std::printf("  rain on a held sheet: %zu drops, added %.4f L; above the sheet %.4f L, more than a cell below it %.4f L\n",
			std::size_t(m.sourceVolume[0]/(4.0/3.0*Pi*std::pow(r.dropRadius, 3)) + 0.5), 1000*m.sourceVolume[0], 1000*above, 1000*below);
		Check(m.sourceVolume[0] > 0 && below < 0.01*m.sourceVolume[0], "rain stays on a held sheet (1% below it)");
	}
}

// ---------------------------------------------------------------- E

std::unique_ptr<ISimulationSolver> Make()
{
	std::string error;
	Check(RegisterLiquidSolver(error), "register", error);
	auto s = CreateSolver(LiquidSolverId, error);
	Check(s != nullptr, "create", error);
	return s;
}
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }

class DigestSink final : public IFrameSink {
public:
	std::map<std::pair<std::int64_t,std::string>,std::string> digests;
	bool Publish(std::int64_t i, const std::string& o, double, std::shared_ptr<const Bytes> p, const Bytes&, std::string&) override {
		digests[{i, o}] = RISE::RISECBOR64::SHA256Hex(*p);
		return true;
	}
};
class DetailProgress final : public IProgress {
public:
	std::string last;
	bool Report(double, double, const std::string& detail) override { last = detail; return true; }
};

V::Members Pool()
{
	return {{"frames", V::Unsigned(10)}, {"frame_rate", V::Float(30)}, {"domain_min", Vec(-0.1, 0, -0.1)}, {"domain_size", Vec(0.2, 0.25, 0.2)},
		{"resolution", V::Unsigned(20)}, {"liquid", V::ArrayValue({V::MapValue({{"type", V::String("fill")}, {"height", V::Float(0.03)}})})}};
}
V Sources()
{
	return V::ArrayValue({V::MapValue({{"name", V::String("nozzle")}, {"shape", V::String("cylinder")}, {"center", Vec(0.02, 0.2, 0)},
		{"axis", Vec(0.3, -1, 0)}, {"radius", V::Float(0.025)}, {"height", V::Float(0.03)}, {"speed", V::Float(0.8)}, {"start", V::Float(0.05)},
		{"end", V::Float(0.25)}, {"schedule", V::ArrayValue({V::Float(0.05), V::Float(0.2), V::Float(0.15), V::Float(1)})}}),
		V::MapValue({{"name", V::String("rain")}, {"shape", V::String("box")}, {"center", Vec(0, 0.22, 0)}, {"size", Vec(0.16, 0.03, 0.16)},
		{"speed", V::Float(1)}, {"drops", V::ArrayValue({V::Unsigned(3), V::Unsigned(3), V::Float(0.012), V::Float(0.1)})}})});
}

bool Bake(const V& params, unsigned threads, DigestSink& sink, std::string& detail, std::string& error, int stopAfter = -1,
	Bytes* checkpoint = nullptr)
{
	auto s = Make();
	V::Members m = params.GetMap();
	m.emplace_back("threads", V::Unsigned(threads));
	SolverDescription d;
	if (!s->Configure(V::MapValue(m), EmptySceneInputs(), d, error) || !s->Start(error)) return false;
	DetailProgress progress;
	AdvanceResult r;
	for (int k = 0; k < 1000; ++k) {
		if (k == stopAfter && checkpoint) {
			if (!s->Checkpoint(*checkpoint, error)) return false;
			auto t = Make();
			SolverDescription d2;
			if (!t->Configure(V::MapValue(m), EmptySceneInputs(), d2, error) || !t->Restore(*checkpoint, error)) return false;
			s = std::move(t);
		}
		if (!s->Advance(AdvanceRequest{}, sink, nullptr, &progress, r, error)) return false;
		if (r.status == AdvanceResult::Status::Finished) { detail = progress.last; return true; }
	}
	error = "did not finish";
	return false;
}

void SolverLevel()
{
	std::printf("E: the solver\n");
	std::string error;
	V::Members base = Pool();
	V::Members with = base;
	with.emplace_back("sources", Sources());
	with.emplace_back("drains", V::ArrayValue({V::MapValue({{"name", V::String("plug")}, {"shape", V::String("box")},
		{"center", Vec(0.08, 0.01, 0.08)}, {"size", Vec(0.03, 0.03, 0.03)}, {"start", V::Float(0.1)}})}));
	DigestSink one, three, resumed;
	std::string detail, d3, dr;
	Check(Bake(V::MapValue(with), 1, one, detail, error), "bake with sources (1 thread)", error);
	Check(Bake(V::MapValue(with), 3, three, d3, error), "bake with sources (3 threads)", error);
	Bytes cp;
	Check(Bake(V::MapValue(with), 3, resumed, dr, error, 4, &cp), "bake with sources, restored from a checkpoint at frame 4", error);
	std::printf("  %zu frames; 1 and 3 threads %s; checkpoint at frame 4 restored %s\n  detail: %s\n", one.digests.size(),
		one.digests == three.digests ? "identical" : "DIFFER", one.digests == resumed.digests ? "identical" : "DIFFERS", detail.c_str());
	Check(!one.digests.empty() && one.digests == three.digests, "frames do not depend on the thread count");
	Check(one.digests == resumed.digests, "a checkpoint restored in the window continues the uninterrupted run");
	Check(detail.find("source nozzle added") != std::string::npos && detail.find("drain plug removed") != std::string::npos &&
		detail.find("liquid volume") != std::string::npos, "the progress detail reports the volumes");
	{
		// Without sources: no new member in the effective parameters, an empty list is the absent one.
		auto s = Make();
		SolverDescription a, b;
		V::Members empty = base;
		empty.emplace_back("sources", V::ArrayValue({}));
		empty.emplace_back("drains", V::ArrayValue({}));
		Check(s->Configure(V::MapValue(base), EmptySceneInputs(), a, error), "configure without sources", error);
		Check(s->Configure(V::MapValue(empty), EmptySceneInputs(), b, error), "configure with empty lists", error);
		Bytes ea, eb;
		RISE::RISECBOR64::Encode(a.effectiveParameters, ea, nullptr);
		RISE::RISECBOR64::Encode(b.effectiveParameters, eb, nullptr);
		Check(ea == eb && !a.effectiveParameters.Find("sources") && !a.effectiveParameters.Find("drains"),
			"without sources the effective parameters are unchanged");
		Check(a.version == "m7m.1", "the solver version is unchanged (m7m.1)", a.version);
		// The effective parameters of this configuration at 9f746162e (m7m.1), pinned.
		const std::string digest = RISE::RISECBOR64::SHA256Hex(ea);
		std::printf("  without sources: effective parameters %s, version %s\n", digest.substr(0, 16).c_str(), a.version.c_str());
		Check(digest == "12556605ea4151538682c92f1438cb550257024465e3070c5c6f6c634bc5d143", "the effective parameters of a configuration without sources are those of 9f746162e", digest);
		SolverDescription c;
		Check(s->Configure(V::MapValue(with), EmptySceneInputs(), c, error), "configure with sources", error);
		Check(c.capabilities.fidelityLimit.find("sources") != std::string::npos || c.evidence.fidelity == "preview", "sources are preview");
		bool condition = false;
		for (const auto& e : c.evidence.conditions) if (e.first == "sources") condition = true;
		Check(condition, "an evidence condition names the sources");
	}
	{
		// Refusals.
		auto s = Make();
		SolverDescription d;
		auto refused = [&](V item, const char* key, const char* what) {
			V::Members m = Pool();
			m.emplace_back(key, V::ArrayValue({item}));
			std::string e;
			Check(!s->Configure(V::MapValue(m), EmptySceneInputs(), d, e), what);
		};
		refused(V::MapValue({{"name", V::String("d")}, {"shape", V::String("box")}, {"speed", V::Float(1)}}), "drains", "a drain with a speed is refused");
		refused(V::MapValue({{"name", V::String("s")}, {"shape", V::String("sphere")}, {"drops", V::ArrayValue({V::Unsigned(2), V::Unsigned(2), V::Float(0.01), V::Float(0.1)})}}),
			"sources", "drops on a sphere are refused");
		refused(V::MapValue({{"name", V::String("s")}, {"shape", V::String("box")}, {"schedule", V::ArrayValue({V::Float(0.2), V::Float(1), V::Float(0.1), V::Float(1)})}}),
			"sources", "a schedule whose times do not increase is refused");
		refused(V::MapValue({{"name", V::String("s")}, {"shape", V::String("box")}, {"start", V::Float(1)}, {"end", V::Float(0.5)}}), "sources",
			"a window that ends before it starts is refused");
		refused(V::MapValue({{"shape", V::String("box")}}), "sources", "a source without a name is refused");
		V::Members two = Pool();
		two.emplace_back("sources", V::ArrayValue({V::MapValue({{"name", V::String("a")}}), V::MapValue({{"name", V::String("a")}})}));
		std::string e;
		Check(!s->Configure(V::MapValue(two), EmptySceneInputs(), d, e), "duplicate source names are refused");
	}
	{
		// The chunk syntax.
		const ParameterSchema& schema = LiquidParameterSchema();
		SchemaFieldValues values;
		values["source"] = {"tap cylinder 0 0.3 0 0 -1 0 0.02 0.04 speed 0.6 start 0.1 end 2 schedule 0 0 0.5 1",
			"cloud box 0 0.5 0 0.4 0.04 0.3 orientation 1 0 0 0 direction 0 -1 0.1 speed 2 drops 8 6 0.01 0.25"};
		values["drain"] = {"plug sphere 0.1 0 0.1 0.03 start 1"};
		SchemaTranslation out;
		Check(schema.translate(values, out, error), "translate sources", error);
		const V* src = nullptr;
		const V* drn = nullptr;
		for (const auto& p : out.parameters) { if (p.first == "sources") src = &p.second; if (p.first == "drains") drn = &p.second; }
		Check(src && src->GetArray().size() == 2 && drn && drn->GetArray().size() == 1, "the chunk's sources and drains");
		if (src && src->GetArray().size() == 2) {
			const V& tap = src->GetArray()[0];
			Check(tap.Find("schedule") && tap.Find("schedule")->GetArray().size() == 4 && tap.Find("height") && tap.Find("axis"), "a nozzle's fields");
			const V& cloud = src->GetArray()[1];
			Check(cloud.Find("drops") && cloud.Find("orientation") && cloud.Find("direction"), "rain's fields");
		}
		SchemaFieldValues bad;
		bad["drain"] = {"plug box 0 0 0 0.1 0.1 0.1 speed 2"};
		SchemaTranslation o2;
		std::string e;
		Check(!schema.translate(bad, o2, e), "a drain's speed is refused by the chunk syntax");
		V::Members m = Pool();
		for (const auto& p : out.parameters) m.emplace_back(p.first, p.second);
		auto s = Make();
		SolverDescription d;
		Check(s->Configure(V::MapValue(m), EmptySceneInputs(), d, error), "the chunk's sources configure", error);
	}
}
}

int main(int argc, char** argv)
{
	const double t0 = Seconds();
	const std::string only = argc > 1 ? argv[1] : "";
	if (only.empty() || only == "A") Accounting();
	if (only.empty() || only == "B") Jets();
	if (only.empty() || only == "C") Rain();
	if (only.empty() || only == "D") WithBodyAndSheet();
	if (only.empty() || only == "E") SolverLevel();
	std::printf("%s (%.1f s)\n", failures ? "FAILED" : "all passed", Seconds() - t0);
	return failures ? 1 : 0;
}
