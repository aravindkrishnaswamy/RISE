// liquid.mac + cloth.shell: sheets coupled implicitly (simulation M7e,
// docs/SIMULATION_M7E_RESULTS.md section 10; preview).  Fast checks A-I run by
// default; --probe (the flap of section 4), --plate, --membrane (mode static |
// freq | model), --copy and --free reproduce the tables of the results.
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidModel.h"
#include "../src/Library/Simulation/Cloth/ClothSolver.h"
#include "../src/Library/Simulation/Cloth/ClothModel.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationGroup.h"
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
using V = RISE::RISECBOR64::Value;
constexpr double Pi = 3.14159265358979323846;
constexpr std::size_t budget = std::size_t(1) << 30;
int failures = 0;
bool verbose = false;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }
double F(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

std::unique_ptr<ISimulationSolver> Make(const char* id)
{
	std::string error;
	auto s = CreateSolver(id, error);
	Check(s != nullptr, std::string("create ") + id, error);
	return s;
}

// ---------------------------------------------------------------- the flap of M7d section 6

struct FlapCase {
	double density = 0.2;      // kg/m^2
	int cells = 40;
	double macro = 1.0/30;
	std::string coupling = "implicit";   // implicit | staggered
	std::string response = "stiffness";  // stiffness | lumped
	double relaxation = 1;
	double T = 1;
	double tilt = 17*Pi/180;
	bool wide = false;         // M7d's flap: 0.44 wide across the 0.4 flume (2 cm outside each wall); else 0.36, inside
	bool free = false;         // a free square sheet 0.24 x 0.24 at (0.5, 0.25, 0.2), tilted by `tilt` from horizontal, no pins
	int vertices = 12;
	double bend = 0;           // > 0: bend_warp = bend_weft (N m)
	bool damping = true;
	double depth = 0.4;        // still water level
	int substeps = 8;          // cloth steps per frame interval
	std::string integrator = "bdf2";
	bool steps = true;         // connect the liquid's steps (the cloth takes them)
	bool elements = true;      // m7h: connect the cloth's surface_elements (the corotational model)
	bool dam = false;          // a dam-break column instead of still water
	unsigned liquidThreads = 8, clothThreads = 4;
	std::vector<std::string> order{"pool", "flap"};
	bool jacobi = false;
};

struct FlapResult {
	bool ok = false, blew = false;
	int steps = 0, blewAt = 0;
	double maxSpeed = 0, maxWet = 0, roughness = 0, seconds = 0;
	std::string error;
	std::vector<double> loads;   // per macro step: the flap's total x force (N)
	std::vector<double> meanVy, meanY;   // per macro step: the cloth's mean vertical velocity and height at the step's start
	double pcg = 0;              // mean PCG iterations per liquid step
	double maxStrain = 0;        // m7h: the largest |edge length / first served length - 1| over the run (macro starts)
};

std::unique_ptr<SimulationGroup> FlapGroupUnstarted(const FlapCase& c, std::string& error);
std::unique_ptr<SimulationGroup> FlapGroup(const FlapCase& c, std::string& error)
{
	auto g = FlapGroupUnstarted(c, error);
	if (g && !g->Start(error)) g.reset();
	return g;
}

std::unique_ptr<SimulationGroup> FlapGroupUnstarted(const FlapCase& c, std::string& error)
{
	const auto frames = std::uint64_t(std::ceil(c.T*30 - 1e-9)) + 1;
	V::Members liquid{{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(1, 0.6, 0.4)},
		{"resolution", V::Unsigned(std::uint64_t(c.cells))}, {"threads", V::Unsigned(c.liquidThreads)}};
	if (c.dam)
		liquid.emplace_back("liquid", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(-1, -1, -1)}, {"max", Vec(0.3, 0.45, 2)}})}));
	else
		liquid.emplace_back("liquid", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(-1, -1, -1)}, {"max", Vec(2, c.depth, 2)}})}));
	V::Members sheet{{"name", V::String("flap")}, {"motion", V::String("port")}};
	if (c.coupling == "implicit") {
		sheet.emplace_back("coupling", V::String("implicit"));
		sheet.emplace_back("response", V::String(c.response));
	} else if (c.relaxation != 1) sheet.emplace_back("relaxation", V::Float(c.relaxation));
	liquid.emplace_back("sheets", V::ArrayValue({V::MapValue(sheet)}));
	V::Members cloth{{"frames", V::Unsigned(frames)}, {"threads", V::Unsigned(c.clothThreads)},
		{"vertices_u", V::Unsigned(std::uint64_t(c.vertices))}, {"vertices_v", V::Unsigned(std::uint64_t(c.vertices))},
		{"damping", V::Bool(c.damping)}, {"substeps", V::Unsigned(std::uint64_t(c.substeps))}, {"integrator", V::String(c.integrator)}};
	{
		V::Members mat{{"base", V::String("cotton-plain-weave")}, {"density", V::Float(c.density)}};
		if (c.bend > 0) { mat.emplace_back("bend_warp", V::Float(c.bend)); mat.emplace_back("bend_weft", V::Float(c.bend)); }
		cloth.emplace_back("material", V::MapValue(mat));
	}
	if (c.free) {
		cloth.emplace_back("size_u", V::Float(0.24));
		cloth.emplace_back("size_v", V::Float(0.24));
		cloth.emplace_back("origin", Vec(0.5 - 0.12*std::cos(c.tilt), 0.25 - 0.12*std::sin(c.tilt), 0.08));
		cloth.emplace_back("axis_u", Vec(std::cos(c.tilt), std::sin(c.tilt), 0));
		cloth.emplace_back("axis_v", Vec(0, 0, 1));
		cloth.emplace_back("pins", V::ArrayValue({}));
	} else {
		cloth.emplace_back("size_u", V::Float(c.wide ? 0.44 : 0.36));
		cloth.emplace_back("size_v", V::Float(0.45));
		cloth.emplace_back("origin", Vec(0.6123, 0.55, c.wide ? -0.02 : 0.02));
		cloth.emplace_back("axis_u", Vec(0, 0, 1));
		cloth.emplace_back("axis_v", Vec(std::sin(c.tilt), -std::cos(c.tilt), 0));
		cloth.emplace_back("pins", V::ArrayValue({V::MapValue({{"select", V::String("v0")}})}));
	}
	GroupPolicy p;
	p.order = c.jacobi ? ExchangeOrder::Jacobi : ExchangeOrder::GaussSeidel;
	if (!c.jacobi) p.memberOrder = c.order;
	p.macroStep = c.macro;
	if (c.coupling == "implicit") p.ownedExchanges = {{"pool", {"flap"}}};
	auto g = std::make_unique<SimulationGroup>();
	bool ok = g->AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), error) &&
		g->AddMember("flap", Make(ClothSolverId), V::MapValue(cloth), error) &&
		g->Connect({"flap", "surface_state", "pool", "sheet_state"}, error) &&
		g->Connect({"pool", "sheet_load", "flap", "surface_load"}, error);
	if (ok && c.coupling == "implicit")
		ok = g->Connect({"flap", "surface_mechanics", "pool", "sheet_mechanics"}, error) &&
			g->Connect({"flap", "surface_stiffness", "pool", "sheet_stiffness"}, error) &&
			(!c.elements || g->Connect({"flap", "surface_elements", "pool", "sheet_elements"}, error)) &&
			(!c.steps || g->Connect({"pool", "sheet_load_steps", "flap", "surface_load_steps"}, error));
	ok = ok && g->Configure(p, EmptySceneInputs(), error);
	if (!ok) g.reset();
	return g;
}

FlapResult RunFlap(const FlapCase& c)
{
	FlapResult r;
	const double t0 = Seconds();
	auto g = FlapGroup(c, r.error);
	if (!g) return r;
	// Connections 0 (state to the liquid) and 1 (load to the cloth).
	std::string e;
	std::vector<double> restEdges;
	while (!g->Finished() && g->Time() < c.T - 1e-12) {
		if (!g->StepMacro(e)) { r.blew = true; r.blewAt = r.steps + 1; r.error = e; break; }
		++r.steps;
		// The cloth's state the liquid read at this step's start, and the load it offered.
		std::shared_ptr<const Bytes> s = g->ServedValue(0), l = g->ServedValue(1);
		if (s) {
			MeshFrame m;
			const StaticPayloadResolver res = [&](const std::string&, std::size_t, std::shared_ptr<const Bytes>&, std::string&) { return false; };
			MeshTopology top;
			std::string why;
			if (PeekMeshTopology(*s, top, why)) {
				// Velocities only need the arrays; decode with the static resolver of the group snapshot.
				auto snap = g->Snapshot("flap");
				const StaticPayloadResolver rs = [&](const std::string& dg, std::size_t bud, std::shared_ptr<const Bytes>& out, std::string& w) {
					return snap->ReadStatic(dg, bud, out, w) == FrameStatus::Ready; };
				if (DecodeMesh(*s, budget, rs, m, why)) {
					// m7h: edge strain against the first served state (the unstretched placement).
					if (restEdges.empty())
						for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3)
							for (int k = 0; k < 3; ++k) {
								const std::uint32_t a = m.indices[t + std::size_t(k)], b = m.indices[t + std::size_t((k + 1) % 3)];
								double l = 0;
								for (int d = 0; d < 3; ++d) { const double e = F(m.positions[3*b + std::uint32_t(d)]) - F(m.positions[3*a + std::uint32_t(d)]); l += e*e; }
								restEdges.push_back(std::sqrt(l));
							}
					else
						for (std::size_t t = 0, q = 0; t + 2 < m.indices.size(); t += 3)
							for (int k = 0; k < 3; ++k, ++q) {
								const std::uint32_t a = m.indices[t + std::size_t(k)], b = m.indices[t + std::size_t((k + 1) % 3)];
								double l = 0;
								for (int d = 0; d < 3; ++d) { const double e = F(m.positions[3*b + std::uint32_t(d)]) - F(m.positions[3*a + std::uint32_t(d)]); l += e*e; }
								if (restEdges[q] > 0) r.maxStrain = std::max(r.maxStrain, std::fabs(std::sqrt(l)/restEdges[q] - 1));
							}
					double my = 0, mv = 0;
					for (std::size_t k = 0; k + 2 < m.velocities.size(); k += 3) { mv += F(m.velocities[k + 1]); my += F(m.positions[k + 1]); }
					r.meanVy.push_back(mv/double(m.velocities.size()/3));
					r.meanY.push_back(my/double(m.velocities.size()/3));
				}
				if (!m.velocities.empty())
					for (std::size_t k = 0; k + 2 < m.velocities.size(); k += 3) {
						const double vx = F(m.velocities[k]), vy = F(m.velocities[k + 1]), vz = F(m.velocities[k + 2]);
						const double sp = std::sqrt(vx*vx + vy*vy + vz*vz);
						if (!std::isfinite(sp)) r.maxSpeed = INFINITY;
						else r.maxSpeed = std::max(r.maxSpeed, sp);
						const double y = F(m.positions[k + 1]), z = F(m.positions[k + 2]);
						if (y < 0.38 && z > 0 && z < 0.4 && std::isfinite(sp)) r.maxWet = std::max(r.maxWet, sp);
					}
				if (verbose && !m.positions.empty()) {
					double ylo = 1e9, yhi = -1e9, xlo = 1e9, xhi = -1e9, vmax = 0; std::size_t at = 0;
					for (std::size_t k = 0; k + 2 < m.positions.size(); k += 3) {
						xlo = std::min(xlo, F(m.positions[k])); xhi = std::max(xhi, F(m.positions[k]));
						ylo = std::min(ylo, F(m.positions[k + 1])); yhi = std::max(yhi, F(m.positions[k + 1]));
						const double vx = F(m.velocities[k]), vy = F(m.velocities[k + 1]), vz = F(m.velocities[k + 2]);
						if (std::sqrt(vx*vx + vy*vy + vz*vz) > vmax) { vmax = std::sqrt(vx*vx + vy*vy + vz*vz); at = k/3; }
					}
					std::printf("    cloth x [%.3f, %.3f] y [%.3f, %.3f], fastest vertex %zu (%.3f, %.3f, %.3f) at %.3g m/s\n", xlo, xhi, ylo, yhi, at,
						F(m.positions[3*at]), F(m.positions[3*at + 1]), F(m.positions[3*at + 2]), vmax);
				}
			}
			(void)res;
		}
		if (l) {
			FieldFrame f;
			std::string why;
			double fx = 0;
			if (DecodeField(*l, budget, f, why))
				for (const auto& ch : f.channels) {
					if (ch.name == "force") for (std::size_t k = 0; k < ch.values.size(); k += 3) fx += F(ch.values[k]);
					else if (ch.name == "pressure") for (std::uint32_t b : ch.values) fx += F(b);
				}
			r.loads.push_back(fx);
		}
		if (verbose) std::printf("  step %d t %.4f max|v| %.4g load %.4g served %d %d\n", r.steps, g->Time(), r.maxSpeed,
			r.loads.empty() ? 0.0 : r.loads.back(), s ? 1 : 0, l ? 1 : 0);
		if (!(r.maxSpeed < 20)) { r.blew = true; r.blewAt = r.steps; break; }
	}
	r.ok = !r.blew;
	double num = 0, den = 0;
	for (std::size_t k = 1; k + 1 < r.loads.size(); ++k) num += std::fabs(r.loads[k + 1] - 2*r.loads[k] + r.loads[k - 1]);
	for (double x : r.loads) den += std::fabs(x);
	r.roughness = den > 0 ? num/den : 0;
	r.seconds = Seconds() - t0;
	return r;
}

// ---------------------------------------------------------------- model-level: a free sheet in a tank

// A square sheet (side a, nd x nd cells) at centre c, tilted by `tilt` about z from horizontal,
// coupled implicitly with lumped masses sigma (kg/m^2) and its weight as the force (no stiffness).
struct FreeSheet {
	Liquid::LiquidModel model;
	bool ok = false;
	std::string error;
};
void MakeFreeSheet(FreeSheet& f, int n, double level, double a, int nd, const Liquid::V3& c, double tilt, double sigma, bool threeD = true)
{
	using namespace Liquid;
	LiquidSetup s;
	s.grid.n = {{n, n, threeD ? n : 1}};
	s.grid.h = 1.0/n;
	s.grid.origin = {{0, 0, threeD ? 0.0 : -0.5/n}};
	Shape col;
	col.type = Shape::Type::Box;
	col.lo = {{-1, -1, -1}};
	col.hi = {{2, level, 2}};
	s.liquid.push_back(col);
	LiquidSheet sh;
	sh.name = "s";
	sh.motion = LiquidSheet::Motion::Port;
	sh.implicit = true;
	sh.dyn.useStiffness = false;
	const double ct = std::cos(tilt), st = std::sin(tilt);
	const double zs = threeD ? a : 2.0/n;   // 2-D: the sheet spans the one cell in z
	for (int j = 0; j <= nd; ++j) for (int i = 0; i <= nd; ++i) {
		const double u = -0.5*a + a*i/nd, w = -0.5*zs + zs*j/nd;
		sh.x.insert(sh.x.end(), {c[0] + u*ct, c[1] + u*st, c[2] + w});
	}
	for (int j = 0; j < nd; ++j) for (int i = 0; i < nd; ++i) {
		const std::uint32_t p = std::uint32_t(i + (nd + 1)*j), q = p + 1, r = p + std::uint32_t(nd + 1) + 1, t = p + std::uint32_t(nd + 1);
		for (std::uint32_t k : {p, q, r, p, r, t}) sh.triangles.push_back(k);
	}
	const std::size_t nv = sh.x.size()/3;
	sh.v.assign(3*nv, 0.0);
	sh.topology = "test";
	sh.dyn.mass.assign(nv, 0.0);
	for (int j = 0; j <= nd; ++j) for (int i = 0; i <= nd; ++i)
		sh.dyn.mass[std::size_t(i + (nd + 1)*j)] = sigma*(a/nd)*(zs/nd)*((i == 0 || i == nd) ? 0.5 : 1)*((j == 0 || j == nd) ? 0.5 : 1);
	sh.dyn.fixed.assign(nv, 0);
	sh.dyn.force.assign(3*nv, 0.0);
	for (std::size_t q = 0; q < nv; ++q) sh.dyn.force[3*q + 1] = -9.81*sh.dyn.mass[q];
	s.sheets.push_back(sh);
	SolverOptions o;
	f.ok = f.model.Initialize(s, o, 8, f.error);
	if (f.ok) { f.model.UpdateSheetGeometry(); f.model.ResetSheetDynamics(); f.ok = f.model.ProjectState(f.error); }
}

void FreeSheetProbe(int argc, char** argv)
{
	int n = 32, nd = 8;
	double sigma = 0.05, tilt = 0, T = 0.5;
	bool threeD = true;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "density") sigma = std::atof(v.c_str());
		else if (k == "cells") n = std::atoi(v.c_str());
		else if (k == "nd") nd = std::atoi(v.c_str());
		else if (k == "tilt") tilt = std::atof(v.c_str())*Pi/180;
		else if (k == "T") T = std::atof(v.c_str());
		else if (k == "2d") threeD = v != "1";
	}
	FreeSheet f;
	MakeFreeSheet(f, n, 0.9, 0.3, nd, {{0.5, 0.5, 0.5}}, tilt, sigma, threeD);
	Check(f.ok, "free sheet", f.error);
	if (!f.ok) return;
	auto& m = f.model;
	const double H = 1.0/30;
	for (double t = 0; t < T - 1e-9; t += H) {
		m.ResetSheetDynamics();
		if (!m.Run(H, H, nullptr, f.error)) { std::printf("failed: %s\n", f.error.c_str()); return; }
		const auto& sh = m.sheets[0];
		double vy = 0, vmax = 0, y = 0;
		for (std::size_t q = 0; q < sh.Vertices(); ++q) {
			vy += sh.v[3*q + 1]/double(sh.Vertices());
			y += sh.x[3*q + 1]/double(sh.Vertices());
			vmax = std::max(vmax, std::sqrt(sh.v[3*q]*sh.v[3*q] + sh.v[3*q + 1]*sh.v[3*q + 1] + sh.v[3*q + 2]*sh.v[3*q + 2]));
		}
		std::printf("  t %.3f mean vy %.4g mean y %.5f max|v| %.4g\n", m.Time(), vy, y, vmax);
		if (t == 0) {
			// The pressure jump across the closed faces against the hydrostatic g dt (scaled), by axis.
			const auto& g = m.GridSpec();
			double sum[3] = {0, 0, 0}, worst[3] = {0, 0, 0};
			int count[3] = {0, 0, 0};
			const double gdt = 9.81*m.lastDt_;
			for (const auto& c : m.SheetFaces()) {
				const auto d = g.FaceDims(c.axis);
				int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])), int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
				const std::size_t R = g.C(idx[0], idx[1], idx[2]);
				--idx[c.axis];
				const std::size_t L = g.C(idx[0], idx[1], idx[2]);
				const double jump = m.q_[L] - m.q_[R] - (c.axis == 1 ? gdt : 0.0);
				sum[c.axis] += jump; worst[c.axis] = std::max(worst[c.axis], std::fabs(jump)); ++count[c.axis];
			}
			for (int a = 0; a < 3; ++a) std::printf("    axis %d: %d faces, mean jump - hydrostatic %.3g, max %.3g (g dt %.3g)\n", a, count[a], count[a] ? sum[a]/count[a] : 0.0, worst[a], gdt);
		}
	}
}

// (a) A stiff free plate released at rest in a tank: its initial acceleration
// a0 = sigma g / (sigma + m_a); the added mass per area m_a = sigma (g / a0 - 1)
// must not depend on sigma.
void PlateProbe(int argc, char** argv)
{
	FlapCase c;
	c.free = true;
	c.tilt = 0;
	c.bend = 1.0;
	c.damping = false;
	c.macro = 1.0/240;
	c.T = 2*c.macro;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "cells") c.cells = std::atoi(v.c_str());
		else if (k == "vertices") c.vertices = std::atoi(v.c_str());
		else if (k == "substeps") c.substeps = std::atoi(v.c_str());
		else if (k == "integrator") c.integrator = v;
		else if (k == "steps") c.steps = v == "1";
		else if (k == "macro") c.macro = 1/std::atof(v.c_str());
		else if (k == "bend") c.bend = std::atof(v.c_str());
	}
	c.T = 2*c.macro + 1e-9;
	for (double sigma : {0.05, 0.2, 1.0, 10.0, 100.0}) {
		c.density = sigma;
		// The frames must reach the second macro step: one frame interval covers it.
		const FlapResult r = RunFlap(c);
		const double a0 = r.meanVy.size() >= 2 ? -r.meanVy[1]/c.macro : NAN;
		std::printf("  sigma %6g: a0 %.5g m/s^2, m_a = sigma (g/a0 - 1) = %.4g kg/m^2 %s\n", sigma, a0, sigma*(9.81/a0 - 1), r.error.c_str());
	}
}

// The liquid's copy of a cloth as the cloth itself: a free cotton-like sheet whose
// linearised response is re-taken from a Cloth::ClothSystem at the copy's state at
// every macro step (no cloth integration: isolates the liquid side).
void CopyOnlyProbe(int argc, char** argv)
{
	using namespace Liquid;
	int n = 24, nv = 12;
	double sigma = 0.05, T = 3, tilt = 0, macro = 1.0/30;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "density") sigma = std::atof(v.c_str());
		else if (k == "cells") n = std::atoi(v.c_str());
		else if (k == "vertices") nv = std::atoi(v.c_str());
		else if (k == "T") T = std::atof(v.c_str());
		else if (k == "tilt") tilt = std::atof(v.c_str())*Pi/180;
		else if (k == "macro") macro = 1/std::atof(v.c_str());
	}
	Cloth::SheetSpec spec;
	spec.sizeU = spec.sizeV = 0.24;
	spec.verticesU = spec.verticesV = std::uint32_t(nv);
	spec.origin = {{0.5 - 0.12*std::cos(tilt), 0.25 - 0.12*std::sin(tilt), 0.08}};
	spec.axisU = {{std::cos(tilt), std::sin(tilt), 0}};
	spec.axisV = {{0, 0, 1}};
	Cloth::ModelConfig cfg;
	Check(Cloth::FindMaterial("cotton-plain-weave", cfg.material), "material");
	cfg.material.density = sigma;
	Cloth::Sheet sheet;
	Cloth::ClothSystem cloth;
	std::string e;
	Check(Cloth::BuildSheet(spec, cfg.material, sheet, e) && cloth.Initialize(sheet, cfg, e), "cloth", e);
	cloth.Reset(0);
	LiquidSetup s;
	s.grid.n = {{n, int(std::lround(0.6*n)), int(std::lround(0.4*n))}};
	s.grid.h = 1.0/n;
	Shape col;
	col.type = Shape::Type::Box;
	col.lo = {{-1, -1, -1}};
	col.hi = {{2, 0.4, 2}};
	s.liquid.push_back(col);
	LiquidSheet sh;
	sh.name = "s";
	sh.motion = LiquidSheet::Motion::Port;
	sh.implicit = true;
	for (const auto& t : cloth.GetSheet().triangles) sh.triangles.insert(sh.triangles.end(), {t.v[0], t.v[1], t.v[2]});
	sh.x = cloth.x;
	sh.v = cloth.v;
	sh.topology = "test";
	if (!std::getenv("M7E_NOSHEET")) s.sheets.push_back(sh);
	LiquidModel m;
	Check(m.Initialize(s, SolverOptions(), 8, e), "liquid", e);
	auto relinearise = [&]() {
		if (m.sheets.empty()) return;
		LiquidSheet& l = m.sheets[0];
		cloth.x = l.dyn.Active() ? l.dyn.xe : l.x;
		cloth.v = l.v;
		l.x = cloth.x;
		std::vector<double> f;
		Cloth::BlockMatrix K;
		cloth.Mechanics(f, K);
		SheetDynamics& d = l.dyn;
		d.mass = cloth.GetSheet().mass;
		d.fixed.assign(d.mass.size(), 0);
		d.force = f;
		d.dampingMass = cfg.material.dampingMass;
		d.dampingStiffness = cfg.material.dampingStiffness;
		d.stiffness = K;
		m.UpdateSheetGeometry(true);
		m.ResetSheetDynamics();
	};
	relinearise();
	Check(m.ProjectState(e), "project", e);
	std::uint64_t pcg = 0, stepCount = 0;
	double wall = 0, project = 0;
	auto after = [&](double, const StepStats& st) { pcg += std::uint64_t(st.pressure.iterations); ++stepCount; project += st.project; };
	for (double t = 0; t < T - 1e-9; t += macro) {
		const double w0 = Seconds();
		if (!m.Run(macro, macro, after, e)) { std::printf("failed: %s\n", e.c_str()); return; }
		wall += Seconds() - w0;
		relinearise();
		if (m.sheets.empty()) continue;
		const auto& l = m.sheets[0];
		double vmax = 0, vy = 0;
		for (std::size_t q = 0; q < l.Vertices(); ++q) {
			vmax = std::max(vmax, std::sqrt(l.v[3*q]*l.v[3*q] + l.v[3*q + 1]*l.v[3*q + 1] + l.v[3*q + 2]*l.v[3*q + 2]));
			vy += l.v[3*q + 1]/double(l.Vertices());
		}
		if (std::lround(m.Time()/macro) % 6 == 0) std::printf("  t %.3f max|v| %.4g mean vy %.4g\n", m.Time(), vmax, vy);
	}
	std::printf("  %llu steps, %.2f PCG iterations per step (two projections), %.4f s per step (projection %.4f)\n", (unsigned long long)stepCount,
		double(pcg)/double(std::max<std::uint64_t>(1, stepCount)), wall/double(std::max<std::uint64_t>(1, stepCount)), project/double(std::max<std::uint64_t>(1, stepCount)));
}

// ---------------------------------------------------------------- membranes (cases b and c)

// A strip membrane across a 2-D tank of span L (pinned at both walls) at height ym, a
// liquid layer of depth d above it, air below.  `pressure`: the cloth's own constant
// pressure on its front (upper) face, pushing down.
struct MembraneCase {
	int cells = 64;            // across the span
	double L = 0.3, ym = 0.05, d = 0.05, height = 0.15;
	double sigma = 0.2;        // kg/m^2
	double E = 2e4;            // stretch_warp = stretch_weft, N/m
	double stretch = 1.01;     // stretch_u of the placement (prestress)
	double g = 9.81;
	double pressure = 0;
	int vertices = 31;
	double T = 1, macro = 1.0/120;
	bool liquid = true;        // false: the cloth alone (vacuum)
	int substeps = 8;
	std::string integrator = "bdf2";
};

V::Members MembraneCloth(const MembraneCase& c, double hz, std::uint64_t frames)
{
	return {{"frames", V::Unsigned(frames)}, {"size_u", V::Float(c.L)}, {"size_v", V::Float(hz)},
		{"vertices_u", V::Unsigned(std::uint64_t(c.vertices))}, {"vertices_v", V::Unsigned(2)},
		{"origin", Vec(0, c.ym, hz)}, {"axis_u", Vec(1, 0, 0)}, {"axis_v", Vec(0, 0, -1)},
		{"stretch_u", V::Float(c.stretch)}, {"gravity", Vec(0, -c.g, 0)}, {"pressure", V::Float(c.pressure)},
		{"damping", V::Bool(false)}, {"threads", V::Unsigned(2)}, {"substeps", V::Unsigned(std::uint64_t(c.substeps))},
		{"integrator", V::String(c.integrator)},
		{"material", V::MapValue({{"base", V::String("cotton-plain-weave")}, {"density", V::Float(c.sigma)},
			{"stretch_warp", V::Float(c.E)}, {"stretch_weft", V::Float(c.E)}})},
		{"pins", V::ArrayValue({V::MapValue({{"select", V::String("u0")}}), V::MapValue({{"select", V::String("u1")}})})}};
}

// The membrane's mid-span height (the mean of its two middle vertices) per macro step.
struct MembraneRun { bool ok = false; std::string error; std::vector<double> t, mid; std::vector<std::vector<double>> shape; };

MembraneRun RunMembrane(const MembraneCase& c)
{
	MembraneRun r;
	const double hz = c.L/c.cells;
	const auto frames = std::uint64_t(std::ceil(c.T*30 - 1e-9)) + 1;
	auto g = std::make_unique<SimulationGroup>();
	std::string& e = r.error;
	bool ok = g->AddMember("membrane", Make(ClothSolverId), V::MapValue(MembraneCloth(c, hz, frames)), e);
	if (c.liquid) {
		V::Members liquid{{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(c.L, c.height, hz)},
			{"resolution", V::Unsigned(std::uint64_t(c.cells))}, {"threads", V::Unsigned(8)}, {"gravity", Vec(0, -c.g, 0)},
			{"volume_correction", V::Bool(std::getenv("M7E_NOVOL") == nullptr)},
			{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(-1, c.ym, -1)}, {"max", Vec(2, c.ym + c.d, 2)}})})},
			{"sheets", V::ArrayValue({V::MapValue({{"name", V::String("m")}, {"motion", V::String("port")}, {"coupling", V::String("implicit")}})})}};
		ok = ok && g->AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), e) &&
			g->Connect({"membrane", "surface_state", "pool", "sheet_state"}, e) &&
			g->Connect({"pool", "sheet_load", "membrane", "surface_load"}, e) &&
			g->Connect({"membrane", "surface_mechanics", "pool", "sheet_mechanics"}, e) &&
			g->Connect({"membrane", "surface_stiffness", "pool", "sheet_stiffness"}, e) &&
			g->Connect({"membrane", "surface_elements", "pool", "sheet_elements"}, e) &&
			g->Connect({"pool", "sheet_load_steps", "membrane", "surface_load_steps"}, e);
	}
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.macroStep = c.macro;
	if (c.liquid) { p.memberOrder = {"pool", "membrane"}; p.ownedExchanges = {{"pool", {"membrane"}}}; }
	ok = ok && g->Configure(p, EmptySceneInputs(), e) && g->Start(e);
	if (!ok) return r;
	// The cloth's state: served to the liquid (connection 0) or, alone, from its published frames.
	while (!g->Finished() && g->Time() < c.T - 1e-12) {
		if (!g->StepMacro(e)) return r;
		std::shared_ptr<const Bytes> s;
		if (c.liquid) s = g->ServedValue(0);
		auto snap = g->Snapshot("membrane");
		const StaticPayloadResolver rs = [&](const std::string& dg, std::size_t bud, std::shared_ptr<const Bytes>& out, std::string& w) {
			return snap->ReadStatic(dg, bud, out, w) == FrameStatus::Ready; };
		if (!s) continue;
		MeshFrame m;
		std::string why;
		if (!DecodeMesh(*s, budget, rs, m, why)) { e = why; return r; }
		const std::size_t a = std::size_t(c.vertices/2), b = std::size_t((c.vertices - 1)/2);
		r.t.push_back(g->Time() - c.macro);   // the state served at this step's start
		r.mid.push_back(0.5*(F(m.positions[3*a + 1]) + F(m.positions[3*b + 1])));
		std::vector<double> sh;
		for (int i = 0; i < c.vertices; ++i) sh.push_back(F(m.positions[3*std::size_t(i) + 1]));
		r.shape.push_back(sh);
	}
	r.ok = true;
	return r;
}

// The cloth alone (Cloth::ClothSystem) in static equilibrium under the analytic
// hydrostatic load of the layer: p = rho g (ys - y) on its upper face, ys from the
// layer's volume L d above the sagged membrane, iterated to a fixed point.
std::vector<double> HydrostaticReference(const MembraneCase& c, double& ys)
{
	using namespace Cloth;
	const double hz = c.L/c.cells;
	SheetSpec spec;
	spec.sizeU = c.L; spec.sizeV = hz;
	spec.verticesU = std::uint32_t(c.vertices); spec.verticesV = 2;
	spec.origin = {{0, c.ym, hz}}; spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, -1}};
	spec.stretchU = c.stretch;
	ModelConfig cfg;
	FindMaterial("cotton-plain-weave", cfg.material);
	cfg.material.density = c.sigma;
	cfg.material.stretchWarp = cfg.material.stretchWeft = c.E;
	cfg.gravity = {{0, -c.g, 0}};
	cfg.damping = false;
	Sheet sheet;
	std::string e;
	BuildSheet(spec, cfg.material, sheet, e);
	PinGroup a, b;
	for (std::uint32_t j = 0; j < 2; ++j) { a.vertices.push_back(sheet.Index(0, j)); b.vertices.push_back(sheet.Index(std::uint32_t(c.vertices - 1), j)); }
	cfg.pins = {a, b};
	ClothSystem cloth;
	cloth.Initialize(sheet, cfg, e);
	cloth.Reset(0);
	const std::uint32_t n = cloth.Vertices();
	ys = c.ym + c.d;
	NewtonOptions o;
	o.tolerance = 1e-9;
	o.maxIterations = 200;
	for (int it = 0; it < 30; ++it) {
		cloth.portPressure.assign(n, 0.0);
		for (std::uint32_t q = 0; q < n; ++q) cloth.portPressure[q] = std::max(0.0, 1000*c.g*(ys - cloth.x[3*q + 1]));
		StepStats st;
		if (!cloth.SolveStatic(o, st, e)) { std::printf("reference static: %s\n", e.c_str()); break; }
		// The level: L d = integral of (ys - y(x)) dx over the span (trapezoid on the vertex row).
		double area = 0;
		for (int i = 0; i + 1 < c.vertices; ++i) {
			const double x0 = cloth.x[3*std::size_t(i)], x1 = cloth.x[3*std::size_t(i + 1)];
			area += (x1 - x0)*0.5*(cloth.x[3*std::size_t(i) + 1] + cloth.x[3*std::size_t(i + 1) + 1]);
		}
		ys = (c.L*c.d + area)/c.L;
	}
	std::vector<double> y;
	for (int i = 0; i < c.vertices; ++i) y.push_back(cloth.x[3*std::size_t(i) + 1]);
	return y;
}

// The membrane at the model level: the liquid with the coupled sheet whose linear model
// is re-taken from the cloth at the copy's state at every macro step (what the group's
// cloth integrates).  For inspecting the liquid.
void MembraneModelProbe(const MembraneCase& c)
{
	using namespace Liquid;
	using namespace Cloth;
	const double hz = c.L/c.cells;
	SheetSpec spec;
	spec.sizeU = c.L; spec.sizeV = hz;
	spec.verticesU = std::uint32_t(c.vertices); spec.verticesV = 2;
	spec.origin = {{0, c.ym, hz}}; spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, -1}};
	spec.stretchU = c.stretch;
	ModelConfig cfg;
	FindMaterial("cotton-plain-weave", cfg.material);
	cfg.material.density = c.sigma;
	cfg.material.stretchWarp = cfg.material.stretchWeft = c.E;
	cfg.gravity = {{0, -c.g, 0}};
	cfg.damping = false;
	cfg.pressure = c.pressure;
	if (std::getenv("M7E_BEND")) cfg.material.bendWarp = cfg.material.bendWeft = std::atof(std::getenv("M7E_BEND"));
	Sheet sheet;
	std::string e;
	BuildSheet(spec, cfg.material, sheet, e);
	PinGroup a, b;
	for (std::uint32_t j = 0; j < 2; ++j) { a.vertices.push_back(sheet.Index(0, j)); b.vertices.push_back(sheet.Index(std::uint32_t(c.vertices - 1), j)); }
	if (!std::getenv("M7E_NOPINS")) cfg.pins = {a, b};
	ClothSystem cloth;
	Check(cloth.Initialize(sheet, cfg, e), "cloth", e);
	cloth.Reset(0);
	LiquidSetup s;
	s.grid.n = {{c.cells, int(std::lround(c.height/hz)), 1}};
	s.grid.h = hz;
	s.gravity = {{0, -c.g, 0}};
	Shape col;
	col.type = Shape::Type::Box;
	col.lo = {{-1, c.ym, -1}};
	col.hi = {{2, c.ym + c.d, 2}};
	s.liquid.push_back(col);
	LiquidSheet sh;
	sh.name = "m";
	sh.motion = LiquidSheet::Motion::Port;
	sh.implicit = true;
	for (const auto& t : cloth.GetSheet().triangles) sh.triangles.insert(sh.triangles.end(), {t.v[0], t.v[1], t.v[2]});
	sh.x = cloth.x; sh.v = cloth.v; sh.topology = "test";
	s.sheets.push_back(sh);
	LiquidModel m;
	SolverOptions so;
	if (std::getenv("M7E_NOPART")) so.particles = false;
	if (std::getenv("M7E_NOVOL")) so.volumeCorrection = false;
	if (std::getenv("M7E_NOREINIT")) so.reinit = ReinitScheme::None;
	if (std::getenv("M7E_STAGES")) so.momentumStages = std::atoi(std::getenv("M7E_STAGES"));
	Check(m.Initialize(s, so, 8, e), "liquid", e);
	auto relinearise = [&]() {
		LiquidSheet& l = m.sheets[0];
		cloth.x = l.dyn.Active() ? l.dyn.xe : l.x;
		cloth.v = l.v;
		if (std::getenv("M7F_ZFIX"))   // diagnostic: no drift of the strip across the 2-D slab
			for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) { cloth.x[3*q + 2] = q < std::uint32_t(c.vertices) ? hz : 0.0; cloth.v[3*q + 2] = 0; l.v[3*q + 2] = 0; }
		l.x = cloth.x;
		std::vector<double> f;
		BlockMatrix K;
		cloth.Mechanics(f, K);
		SheetDynamics& d = l.dyn;
		d.mass = cloth.GetSheet().mass;
		d.fixed.assign(d.mass.size(), 0);
		for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) d.fixed[q] = cloth.Fixed(q) || std::getenv("M7E_PINALL");
		if (std::getenv("M7E_PINALL")) std::fill(l.v.begin(), l.v.end(), 0.0);
		d.force = f;
		d.stiffness = K;
		m.UpdateSheetGeometry(true);
		m.ResetSheetDynamics();
	};
	relinearise();
	Check(m.ProjectState(e), "project", e);
	const auto& g = m.GridSpec();
	auto membraneAt = [&](double x) {
		const auto& l = m.sheets[0];
		const int seg = std::min(c.vertices - 2, std::max(0, int(x/c.L*(c.vertices - 1))));
		const double t0 = x/c.L*(c.vertices - 1) - seg;
		return (1 - t0)*l.x[3*std::size_t(seg) + 1] + t0*l.x[3*std::size_t(seg + 1) + 1];
	};
	std::vector<std::uint8_t> wetBelow(g.Cells(), 0);
	std::vector<double> mids, times;
	const bool watch = std::getenv("M7E_WATCH") != nullptr;
	for (double t = 0; t < c.T - 1e-9; t += c.macro) {
		if (!watch) {
			const int sub = std::getenv("M7E_SUB") ? std::atoi(std::getenv("M7E_SUB")) : 1;
			// M7F_FLOOD: after every step, whether the open faces connect the top row to a cell
			// whose centre lies below the membrane (a leak path through the pressure solve).
			auto flood = [&](double, const Liquid::StepStats&) {
				std::vector<std::uint8_t> seen(g.Cells(), 0);
				std::vector<std::size_t> stack;
				for (int i = 0; i < g.n[0]; ++i) { const std::size_t k = g.C(i, g.n[1] - 1, 0); seen[k] = 1; stack.push_back(k); }
				int below = 0;
				while (!stack.empty()) {
					const std::size_t k = stack.back(); stack.pop_back();
					const int i = int(k % std::size_t(g.n[0])), j = int(k/std::size_t(g.n[0]));
					if ((j + 0.5)*hz < membraneAt((i + 0.5)*hz) - 1e-9) ++below;
					const int nb[4][3] = {{i - 1, j, 0}, {i + 1, j, 1}, {i, j - 1, 2}, {i, j + 1, 3}};
					for (const auto& q : nb) {
						if (q[0] < 0 || q[0] >= g.n[0] || q[1] < 0 || q[1] >= g.n[1]) continue;
						const int a = q[2] < 2 ? 0 : 1;
						const std::size_t f = a == 0 ? g.F(0, std::max(i, q[0]), j, 0) : g.F(1, i, std::max(j, q[1]), 0);
						if (!(m.fraction[std::size_t(a)][f] > 0)) continue;
						const std::size_t kk = g.C(q[0], q[1], 0);
						if (!seen[kk]) { seen[kk] = 1; stack.push_back(kk); }
					}
				}
				if (below) std::printf("    t %.4f: %d cells below the membrane reachable from the top\n", m.Time(), below);
				if (below > 50 && std::getenv("M7F_FLOOD")[0] == '2') {
					// Per column: the closed y faces and the sheet's two vertex rows there.
					const auto& l = m.sheets[0];
					for (int i = 0; i < g.n[0]; ++i) {
						int closed = 0, first = -1;
						for (int j = 1; j < g.n[1]; ++j) if (!(m.fraction[1][g.F(1, i, j, 0)] > 0)) { ++closed; if (first < 0) first = j; }
						if (closed != 1) std::printf("      column %d: %d closed y faces (first %d), membrane %.5f\n", i, closed, first, membraneAt((i + 0.5)*hz));
					}
					for (int q = 0; q < c.vertices; ++q) std::printf("      v%d (%.4f %.5f %.4f) (%.4f %.5f %.4f)\n", q, l.x[3*q], l.x[3*q + 1], l.x[3*q + 2],
						l.x[3*(q + c.vertices)], l.x[3*(q + c.vertices) + 1], l.x[3*(q + c.vertices) + 2]);
					std::exit(0);
				}
			};
			if (!m.Run(c.macro, c.macro/sub, std::getenv("M7F_FLOOD") ? std::function<void(double, const Liquid::StepStats&)>(flood) : nullptr, e)) { std::printf("failed: %s\n", e.c_str()); return; }
			mids.push_back(m.sheets[0].x[3*std::size_t(c.vertices/2) + 1]);
			times.push_back(m.Time());
			if (std::getenv("M7E_PISTON")) {
				const auto& l = m.sheets[0];
				double vm = 0;
				for (std::size_t q = 0; q < l.Vertices(); ++q) vm += l.v[3*q + 1]/double(l.Vertices());
				const V3 P = m.FluidMomentum();
				// Kinetic energies: the liquid's (the projection's weights) and the membrane's.
				double ke = 0;
				for (int a = 0; a < 2; ++a) {
					const auto d = g.FaceDims(a);
					for (int jj = 0; jj < d[1]; ++jj) for (int ii = 0; ii < d[0]; ++ii) {
						const int idx[3] = {ii, jj, 0};
						if (idx[a] == 0 || idx[a] == g.n[a]) continue;
						const std::size_t f = g.F(a, ii, jj, 0);
						const double A = m.fraction[std::size_t(a)][f];
						if (!(A > 0)) continue;
						int lo[3] = {ii, jj, 0};
						--lo[a];
						const double pl = m.phi[g.C(lo[0], lo[1], 0)], pr = m.phi[g.C(ii, jj, 0)];
						double w = 0;
						if (pl < 0 && pr < 0) w = A; else if (pl < 0) w = A*std::min(1.0, pl/(pl - pr)); else if (pr < 0) w = A*std::min(1.0, pr/(pr - pl));
						ke += 0.5*1000*hz*hz*hz*w*m.u.u[std::size_t(a)][f]*m.u.u[std::size_t(a)][f];
					}
				}
				double kem = 0, wd = 0;
				for (std::size_t q = 0; q < l.Vertices(); ++q) {
					kem += 0.5*l.dyn.mass[q]*l.v[3*q + 1]*l.v[3*q + 1];
					if (q == std::size_t(c.vertices/2)) wd = l.v[3*q + 1];
				}
				{
					// The pressure in the column above the middle of the sheet.
					const int i = g.n[0]/2;
					std::printf("    pressure column at i=%d:", i);
					for (int j = 0; j < g.n[1]; ++j) if (m.phi[g.C(i, j, 0)] < 0) std::printf(" %d:%.4g", j, m.Pressure(g.C(i, j, 0)));
					double iy = 0, fy = 0, my = 0;
					for (std::size_t q = 0; q < l.Vertices(); ++q) { iy += m.sheetImpulse[0][3*q + 1]; fy += l.dyn.force[3*q + 1]; my += l.dyn.mass[q]*l.v[3*q + 1]; }
					std::printf("  (sheet y %.5f, lastDt %.5g); impulse on the cloth y %.4g, its force f0 y %.4g x dt %.4g, momentum %.4g\n",
						l.x[3*std::size_t(c.vertices/2) + 1], m.lastDt_, iy, fy, fy*m.lastDt_, my);
				}
				std::printf("    t %.4f membrane mean vy %.4g, liquid momentum y %.4g (rho d L x that: %.4g); KE liquid %.4g membrane %.4g, mid speed %.4g: M_eff %.4g kg/m\n", m.Time(), vm, P[1],
					1000*c.d*c.L*hz*vm, ke, kem, wd, (ke + kem)/(0.5*wd*wd)/hz);
			}
		}
		else {
			// Step by step, reporting dry-side cells (centre more than h/2 below the membrane) that turn liquid.
			std::vector<double> before = m.phi;
			int shown = 0;
			auto check = [&](double, const Liquid::StepStats& st) {
			for (int i = 0; i < g.n[0] && shown < 4; ++i) for (int j = 0; j < g.n[1] && shown < 4; ++j) {
				const std::size_t k = g.C(i, j, 0);
				const double y = (j + 0.5)*hz, ymb = membraneAt((i + 0.5)*hz);
				const bool wb = m.phi[k] < 0 && y < ymb - (std::getenv("M7E_ANY") ? 0.0 : 0.5*hz);
				if (wb && !wetBelow[k]) {
					std::printf("    step t %.4f cell (%d, %d) y %.4f membrane %.4f: phi %.3g -> %.3g; above %.3g -> %.3g, side %.3g %.3g; swept %zu kept %zu\n", m.Time(), i, j, y, ymb,
						before[k], m.phi[k], before[g.C(i, j + 1, 0)], m.phi[g.C(i, j + 1, 0)], i > 0 ? m.phi[g.C(i - 1, j, 0)] : 9.0, i + 1 < g.n[0] ? m.phi[g.C(i + 1, j, 0)] : 9.0,
						st.sheetCellsKept, st.sheetParticlesRemoved);
					++shown;
				}
				wetBelow[k] = wb;
			}
			before = m.phi;
			};
			if (!m.Run(c.macro, c.macro, check, e)) { std::printf("failed: %s\n", e.c_str()); return; }
			for (int i = 0; i < g.n[0] && shown < 4; ++i) for (int j = 0; j < g.n[1] && shown < 4; ++j) {
				const std::size_t k = g.C(i, j, 0);
				const double y = (j + 0.5)*hz, ymb = membraneAt((i + 0.5)*hz);
				const bool wb = m.phi[k] < 0 && y < ymb - 0.5*hz;
				if (wb && !wetBelow[k]) {
					std::printf("    t %.4f cell (%d, %d) y %.4f membrane %.4f: phi %.3g -> %.3g; above %.3g -> %.3g, side %.3g %.3g\n", m.Time(), i, j, y, ymb,
						before[k], m.phi[k], before[g.C(i, j + 1, 0)], m.phi[g.C(i, j + 1, 0)], i > 0 ? m.phi[g.C(i - 1, j, 0)] : 9.0, i + 1 < g.n[0] ? m.phi[g.C(i + 1, j, 0)] : 9.0);
					++shown;
				}
				wetBelow[k] = wb;
			}
		}
		relinearise();
		if (std::lround(m.Time()/c.macro) % (std::getenv("M7E_EVERY") ? 1 : 12) != 0) continue;
		// Liquid below the membrane (cells whose centre is below the membrane's height there).
		const auto& l = m.sheets[0];
		double below = 0, total = 0, mid = 0;
		for (int i = 0; i < g.n[0]; ++i) {
			const double x = (i + 0.5)*hz;
			const int seg = std::min(c.vertices - 2, int(x/c.L*(c.vertices - 1)));
			const double t0 = x/c.L*(c.vertices - 1) - seg;
			const double ym = (1 - t0)*l.x[3*std::size_t(seg) + 1] + t0*l.x[3*std::size_t(seg + 1) + 1];
			for (int j = 0; j < g.n[1]; ++j) {
				const double ph = m.phi[g.C(i, j, 0)];
				const double H = std::min(1.0, std::max(0.0, 0.5 - ph/hz))*hz*hz;
				total += H;
				if ((j + 0.5)*hz < ym) below += H;
			}
		}
		mid = l.x[3*std::size_t(c.vertices/2) + 1];
		std::printf("  t %.3f mid sag %.5f liquid below %.3g of %.3g (m^2)\n", m.Time(), c.ym - mid, below, total);
	}
	if (mids.size() > 4) {
		double mean = 0;
		for (double x : mids) mean += x/double(mids.size());
		std::vector<double> zc;
		for (std::size_t j = 1; j < mids.size(); ++j)
			if ((mids[j - 1] - mean > 0) != (mids[j] - mean > 0))
				zc.push_back(times[j - 1] + (times[j] - times[j - 1])*(mids[j - 1] - mean)/(mids[j - 1] - mids[j]));
		std::printf("  model: %zu crossings, omega %.4g rad/s\n", zc.size(), zc.size() > 2 ? Pi*double(zc.size() - 1)/(zc.back() - zc.front()) : NAN);
	}
}

// (c) The fundamental of the strip with the layer above it and no gravity (the free
// surface at p = 0), derived here from linear potential flow: with w = W sin(k x), k =
// pi / L, and the layer's potential expanded in the wall modes cos(n pi x / L), the
// liquid's kinetic energy is M_a W'^2 / 2 with
//     M_a = rho L [a0^2 d + (1/2) sum_{n even >= 2} a_n^2 tanh(k_n d) / k_n],
//     a0 = 2 / pi, a_n = 4 / (pi (1 - n^2)), k_n = n pi / L,
// so omega^2 = omega_vac^2 (sigma L / 2) / (sigma L / 2 + M_a), omega_vac^2 = T k^2 / sigma
// the strip's own fundamental, taken from the cloth model's stiffness at its flat,
// prestressed state (the Rayleigh quotient of the sampled sine, exact for a uniform string).
double MembraneFrequencyReference(const MembraneCase& c)
{
	using namespace Cloth;
	const double hz = c.L/c.cells;
	SheetSpec spec;
	spec.sizeU = c.L; spec.sizeV = hz;
	spec.verticesU = std::uint32_t(c.vertices); spec.verticesV = 2;
	spec.origin = {{0, c.ym, hz}}; spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, -1}};
	spec.stretchU = c.stretch;
	ModelConfig cfg;
	FindMaterial("cotton-plain-weave", cfg.material);
	cfg.material.density = c.sigma;
	cfg.material.stretchWarp = cfg.material.stretchWeft = c.E;
	cfg.gravity = {{0, 0, 0}};
	Sheet sheet;
	std::string e;
	BuildSheet(spec, cfg.material, sheet, e);
	ClothSystem cloth;
	cloth.Initialize(sheet, cfg, e);
	cloth.Reset(0);
	std::vector<double> f;
	BlockMatrix K;
	cloth.Mechanics(f, K);
	std::vector<double> w(cloth.x.size(), 0.0), kw;
	for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) w[3*q + 1] = std::sin(Pi*cloth.x[3*q]/c.L);
	K.Multiply(w, kw);
	double num = 0, den = 0;
	for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) { num += w[3*q + 1]*kw[3*q + 1]; den += cloth.GetSheet().mass[q]*w[3*q + 1]*w[3*q + 1]; }
	const double wvac2 = num/den;
	double Ma = 1000*c.L*(4/(Pi*Pi))*c.d, sum = 0;
	for (int n = 2; n < 20000; n += 2) {
		const double an = 4/(Pi*(1.0 - double(n)*n)), kn = n*Pi/c.L;
		sum += an*an*std::tanh(kn*c.d)/kn;
	}
	Ma += 1000*c.L*0.5*sum;
	const double m = c.sigma*c.L/2;
	std::printf("  reference: omega_vac %.5g rad/s (T = %.4g N/m), M_a %.4g kg/m (sigma L / 2 = %.4g)\n", std::sqrt(wvac2),
		wvac2*c.sigma/((Pi/c.L)*(Pi/c.L)), Ma, m);
	return std::sqrt(wvac2*m/(m + Ma));
}

void MembraneProbe(int argc, char** argv)
{
	MembraneCase c;
	std::string mode = "static";
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "cells") c.cells = std::atoi(v.c_str());
		else if (k == "density") c.sigma = std::atof(v.c_str());
		else if (k == "mode") mode = v;
		else if (k == "T") c.T = std::atof(v.c_str());
		else if (k == "macro") c.macro = 1/std::atof(v.c_str());
		else if (k == "E") c.E = std::atof(v.c_str());
		else if (k == "stretch") c.stretch = std::atof(v.c_str());
		else if (k == "vertices") c.vertices = std::atoi(v.c_str());
		else if (k == "pressure") c.pressure = std::atof(v.c_str());
		else if (k == "g") c.g = std::atof(v.c_str());
		else if (k == "depth") c.d = std::atof(v.c_str());
		else if (k == "ym") c.ym = std::atof(v.c_str());
	}
	if (mode == "model") { MembraneModelProbe(c); return; }
	if (mode == "static") {
		double ys = 0;
		const auto ref = HydrostaticReference(c, ys);
		const MembraneRun r = RunMembrane(c);
		if (!r.ok) { std::printf("failed: %s\n", r.error.c_str()); return; }
		const std::size_t a = std::size_t(c.vertices/2), b = std::size_t((c.vertices - 1)/2);
		const double refMid = 0.5*(ref[a] + ref[b]);
		double mean = 0; int k = 0;
		for (std::size_t j = 0; j < r.t.size(); ++j) if (r.t[j] >= 0.5*c.T) { mean += r.mid[j]; ++k; }
		mean /= std::max(1, k);
		double lo = 1e9, hi = -1e9;
		for (std::size_t j = 0; j < r.t.size(); ++j) if (r.t[j] >= 0.5*c.T) { lo = std::min(lo, r.mid[j]); hi = std::max(hi, r.mid[j]); }
		std::printf("cells %d sigma %g: sag (mean of the second half) %.5f m, reference %.5f m (level %.4f), error %.3g of the sag; oscillation +-%.2g\n",
			c.cells, c.sigma, c.ym - mean, c.ym - refMid, ys, ((c.ym - mean) - (c.ym - refMid))/(c.ym - refMid), 0.5*(hi - lo));
	} else {
		// Frequency of the mid-span displacement about its mean (zero crossings).
		MembraneCase v = c;
		const MembraneRun r = RunMembrane(c);
		if (!r.ok) { std::printf("failed: %s\n", r.error.c_str()); return; }
		double mean = 0;
		for (double x : r.mid) mean += x/double(r.mid.size());
		std::vector<double> zc;
		for (std::size_t j = 1; j < r.mid.size(); ++j)
			if ((r.mid[j - 1] - mean > 0) != (r.mid[j] - mean > 0))
				zc.push_back(r.t[j - 1] + (r.t[j] - r.t[j - 1])*(r.mid[j - 1] - mean)/(r.mid[j - 1] - r.mid[j]));
		const double w = zc.size() > 2 ? Pi*double(zc.size() - 1)/(zc.back() - zc.front()) : NAN;
		const double wr = MembraneFrequencyReference(c);
		std::printf("cells %d sigma %g: %zu crossings, omega %.4g rad/s, reference %.4g, error %.3g\n", c.cells, c.sigma, zc.size(), w, wr, w/wr - 1);
	}
}


// ---------------------------------------------------------------- model-level rig and the fast checks

// The strip membrane of MembraneCase at the model level: the liquid with the coupled
// sheet whose linear model is re-taken from a Cloth::ClothSystem at the copy's state
// every macro step (what the group's cloth integrates, without the exchange).
struct MembraneRig {
	MembraneCase c;
	Cloth::ClothSystem cloth;
	Liquid::LiquidModel m;
	double hz = 0;
	bool ok = false;
	std::string error;
	bool Build(const MembraneCase& cc, bool pins, double bend, unsigned threads = 8) {
		using namespace Cloth;
		c = cc;
		hz = c.L/c.cells;
		SheetSpec spec;
		spec.sizeU = c.L; spec.sizeV = hz;
		spec.verticesU = std::uint32_t(c.vertices); spec.verticesV = 2;
		spec.origin = {{0, c.ym, hz}}; spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, -1}};
		spec.stretchU = c.stretch;
		ModelConfig cfg;
		FindMaterial("cotton-plain-weave", cfg.material);
		cfg.material.density = c.sigma;
		cfg.material.stretchWarp = cfg.material.stretchWeft = c.E;
		if (bend > 0) cfg.material.bendWarp = cfg.material.bendWeft = bend;
		cfg.gravity = {{0, -c.g, 0}};
		cfg.damping = false;
		cfg.pressure = c.pressure;
		Sheet sheet;
		if (!BuildSheet(spec, cfg.material, sheet, error)) return false;
		PinGroup a, b;
		for (std::uint32_t j = 0; j < 2; ++j) { a.vertices.push_back(sheet.Index(0, j)); b.vertices.push_back(sheet.Index(std::uint32_t(c.vertices - 1), j)); }
		if (pins) cfg.pins = {a, b};
		if (!cloth.Initialize(sheet, cfg, error)) return false;
		cloth.Reset(0);
		using namespace Liquid;
		LiquidSetup s;
		s.grid.n = {{c.cells, int(std::lround(c.height/hz)), 1}};
		s.grid.h = hz;
		s.gravity = {{0, -c.g, 0}};
		if (c.d > 0) {
			Shape col;
			col.type = Shape::Type::Box;
			col.lo = {{-1, c.ym, -1}};
			col.hi = {{2, c.ym + c.d, 2}};
			s.liquid.push_back(col);
		}
		LiquidSheet sh;
		sh.name = "m";
		sh.motion = LiquidSheet::Motion::Port;
		sh.implicit = true;
		for (const auto& t : cloth.GetSheet().triangles) sh.triangles.insert(sh.triangles.end(), {t.v[0], t.v[1], t.v[2]});
		sh.x = cloth.x; sh.v = cloth.v; sh.topology = "test";
		s.sheets.push_back(sh);
		if (!m.Initialize(s, SolverOptions(), threads, error)) return false;
		Relinearise();
		ok = m.ProjectState(error);
		return ok;
	}
	void Relinearise() {
		using namespace Liquid;
		LiquidSheet& l = m.sheets[0];
		cloth.x = l.dyn.Active() ? l.dyn.xe : l.x;
		cloth.v = l.v;
		l.x = cloth.x;
		std::vector<double> f;
		Cloth::BlockMatrix K;
		cloth.Mechanics(f, K);
		SheetDynamics& d = l.dyn;
		d.mass = cloth.GetSheet().mass;
		d.fixed.assign(d.mass.size(), 0);
		for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) d.fixed[q] = cloth.Fixed(q);
		d.force = f;
		d.stiffness = K;
		m.UpdateSheetGeometry(true);
		m.ResetSheetDynamics();
	}
	bool Macro() {
		if (!m.Run(c.macro, c.macro, nullptr, error)) return false;
		Relinearise();
		return true;
	}
	double Mid() const { return m.sheets[0].x[3*std::size_t(c.vertices/2) + 1]; }
	//! Liquid (open faces), the liquid on the sheet's closed faces, and the sheet: vertical momentum per slab (N s).
	double Momentum() const {
		using namespace Liquid;
		const auto& g = m.GridSpec();
		const LiquidSheet& l = m.sheets[0];
		double p = m.FluidMomentum()[1];
		const double scale = m.Setup().density*g.h*g.h*g.h;
		for (std::size_t k = 0; k < m.SheetFaces().size(); ++k) {
			const SheetCrossing& x = m.SheetFaces()[k];
			if (x.axis != 1) continue;
			const auto d = g.FaceDims(1);
			int idx[3] = {int(x.face % std::size_t(d[0])), int((x.face/std::size_t(d[0])) % std::size_t(d[1])), 0};
			const double pr = m.phi[g.C(idx[0], idx[1], 0)];
			--idx[1];
			const double pl = m.phi[g.C(idx[0], idx[1], 0)];
			double theta = 0;
			if (pl < 0 && pr < 0) theta = 1; else if (pl < 0) theta = std::min(1.0, std::max(1e-3, pl/(pl - pr))); else if (pr < 0) theta = std::min(1.0, std::max(1e-3, pr/(pr - pl)));
			p += scale*theta*x.velocity;
		}
		for (std::size_t q = 0; q < l.Vertices(); ++q) p += l.dyn.mass[q]*l.v[3*q + 1];
		return p;
	}
};

// ---------------------------------------------------------------- m7f: a pinned sheet holding liquid (3-D)

// A square tank L x H x L, a sheet spanning it at height y0 (on a face plane), pinned along
// the four walls, a layer d of liquid on it, air below: a watertight bag.  Liquid can reach
// the space below only through the sheet.  The leak is measured topologically: the cells
// reachable from the top row through open faces and those reachable from the bottom row are
// two components while the barrier holds ("breached" if they meet); the liquid in each is the
// sharp count clamp(1/2 - phi/h, 0, 1) h^3, independent of the volume correction (whose own
// contribution is tallied separately).
struct CupCase {
	int cells = 40;            // across the tank
	double L = 0.3, H = 0.3, y0 = 0.2, d = 0.03;
	double sigma = 0.15;       // kg/m^2 (cotton-plain-weave's own)
	double E = 0;              // > 0: stretch_warp = stretch_weft override (N/m)
	int vertices = 21;
	double T = 2, macro = 1.0/120;
	bool volume = true, particles = true, damping = true, reinit = true;
	unsigned threads = 8;
	bool frozen = false;       // m7j: every vertex pinned (a static sheet)
	double offset = 0;         // m7j: the sheet's height above the face plane (cells)
	double freeze = -1;        // m7j: from this time on the sheet is held where it is (every vertex fixed, at rest)
	double u0 = 0;             // m7j: the liquid's initial velocity along x
	bool bowl = false;         // m7j: the sheet held in a spherical bowl from the start (needs frozen), the layer d above its rim
	bool coupled = true;       // m7j: false: the sheet is a moving boundary only (m7d's path)
};
struct CupSample { double t = 0, top = 0, bottom = 0, bottomWet = 0, beyond = 0, other = 0, smoothed = 0, added = 0, sag = 0, vmax = 0; bool breached = false; };

struct CupRig {
	CupCase c;
	Cloth::ClothSystem cloth;
	Liquid::LiquidModel m;
	double h = 0, y0 = 0, added = 0, lastStageVolume = -1;
	std::map<std::string, double> stageSums;   // M7F_STAGES: per stage, the volume change it made (m^3); "advected" includes the step's end
	std::size_t kept = 0;
	std::string error;
	bool Build(const CupCase& cc) {
		using namespace Cloth;
		c = cc;
		h = c.L/c.cells;
		y0 = h*(std::round(c.y0/h) + c.offset);
		SheetSpec spec;
		spec.sizeU = c.L; spec.sizeV = c.L;
		spec.verticesU = spec.verticesV = std::uint32_t(c.vertices);
		spec.origin = {{0, y0, c.L}}; spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 0, -1}};
		ModelConfig cfg;
		FindMaterial("cotton-plain-weave", cfg.material);
		cfg.material.density = c.sigma;
		if (c.E > 0) cfg.material.stretchWarp = cfg.material.stretchWeft = c.E;
		cfg.gravity = {{0, -9.81, 0}};
		cfg.damping = c.damping;
		Sheet sheet;
		if (!BuildSheet(spec, cfg.material, sheet, error)) return false;
		PinGroup rim;
		const std::uint32_t n = std::uint32_t(c.vertices);
		for (std::uint32_t j = 0; j < n; ++j) for (std::uint32_t i = 0; i < n; ++i)
			if (c.frozen || i == 0 || j == 0 || i + 1 == n || j + 1 == n) rim.vertices.push_back(sheet.Index(i, j));
		cfg.pins = {rim};
		if (!cloth.Initialize(sheet, cfg, error)) return false;
		cloth.Reset(0);
		// m7j: the sheet held in a spherical bowl (sagitta 0.03 m over the half width, rim at y0; every vertex pinned).
		const double bowlR = (0.25*c.L*c.L + 0.03*0.03)/(2*0.03), bowlY = y0 - 0.03 + bowlR;
		auto bowlAt = [&](double x, double z) { const double dx = x - 0.5*c.L, dz = z - 0.5*c.L; return bowlY - std::sqrt(std::max(0.0, bowlR*bowlR - dx*dx - dz*dz)); };
		if (c.bowl) for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) cloth.x[3*q + 1] = bowlAt(cloth.x[3*q], cloth.x[3*q + 2]);
		using namespace Liquid;
		LiquidSetup s;
		s.grid.n = {{c.cells, int(std::lround(c.H/h)), c.cells}};
		s.grid.h = h;
		Shape col;
		col.type = Shape::Type::Box;
		col.lo = {{-1, y0, -1}};
		col.hi = {{2, y0 + c.d, 2}};
		if (c.bowl) col.lo[1] = -1;
		s.initialVelocity = {{c.u0, 0, 0}};
		s.liquid.push_back(col);
		LiquidSheet sh;
		sh.name = "bag";
		sh.motion = LiquidSheet::Motion::Port;
		sh.implicit = c.coupled;
		for (const auto& t : cloth.GetSheet().triangles) sh.triangles.insert(sh.triangles.end(), {t.v[0], t.v[1], t.v[2]});
		sh.x = cloth.x; sh.v = cloth.v; sh.topology = "test";
		s.sheets.push_back(sh);
		SolverOptions so;
		so.volumeCorrection = c.volume;
		so.particles = c.particles;
		if (!c.reinit) so.reinit = ReinitScheme::None;
		if (!m.Initialize(s, so, c.threads, error)) return false;
		if (std::getenv("M7F_STAGES")) {
			// Per stage of the step: the change of the smoothed volume it made, summed over the run
			// (m7j, M7J_SHARP: of the sharp volume sum clamp(1/2 - phi/h, 0, 1) h^3).
			const double from = std::atof(std::getenv("M7F_STAGES"));
			m.stageHook = [this, from](const char* stage) {
				if (std::getenv("M7J_TRACE")) Trace(stage);
				const double v = std::getenv("M7J_SHARP") ? SharpVolume() : m.Volume();
				if (lastStageVolume >= 0 && m.Time() >= from) stageSums[stage] += v - lastStageVolume;
				lastStageVolume = v;
			};
		}
		if (std::getenv("M7G_DEEP")) {
			// m7g diagnostic: the region well below the sheet (y < y0 - 0.055 m, below any sag): per
			// stage, the change of its smoothed volume (1 - H(phi)) h^3, summed over the run, and the
			// negative marker particles there at each macro step (Sample).
			m.stageHook = [this](const char* stage) {
				const double v = DeepVolume();
				if (lastStageVolume >= 0) stageSums[stage] += v - lastStageVolume;
				lastStageVolume = v;
				// The floor region (the lowest 10 rows): the largest fall of phi in one stage.
				const auto& g = m.GridSpec();
				if (lastPhi.size() == m.phi.size()) {
					double worst = 0;
					std::size_t at = 0;
					for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < std::min(10, g.n[1]); ++j) for (int i = 0; i < g.n[0]; ++i) {
						const std::size_t q = g.C(i, j, k);
						const double fall = lastPhi[q] - m.phi[q];
						if (fall > worst) { worst = fall; at = q; }
					}
					if (worst > 0.2*g.h) {
						const int i = int(at % std::size_t(g.n[0])), j = int((at/std::size_t(g.n[0])) % std::size_t(g.n[1])), k = int(at/(std::size_t(g.n[0])*std::size_t(g.n[1])));
						std::printf("    FALL t %.4f stage %s cell (%d %d %d) %.3f -> %.3f h; nbrs x %.2f %.2f y %.2f %.2f z %.2f %.2f\n", m.Time(), stage, i, j, k,
							lastPhi[at]/g.h, m.phi[at]/g.h, i > 0 ? m.phi[at - 1]/g.h : 99.0, i + 1 < g.n[0] ? m.phi[at + 1]/g.h : 99.0,
							j > 0 ? m.phi[g.C(i, j - 1, k)]/g.h : 99.0, m.phi[g.C(i, j + 1, k)]/g.h, k > 0 ? m.phi[g.C(i, j, k - 1)]/g.h : 99.0, k + 1 < g.n[2] ? m.phi[g.C(i, j, k + 1)]/g.h : 99.0);
					}
				}
				lastPhi = m.phi;
				if (const char* col = std::getenv("M7G_COL")) {
					// "i k t0 t1": the column (i, *, k) and its two wall-side neighbours' least value, per stage within [t0, t1].
					int ci = 0, ck = 0;
					double t0 = 0, t1 = 0;
					if (std::sscanf(col, "%d %d %lf %lf", &ci, &ck, &t0, &t1) == 4 && m.Time() >= t0 && m.Time() <= t1) {
						std::printf("    COL t %.5f %-13s", m.Time(), stage);
						for (int j = 0; j < g.n[1]; ++j) {
							const bool closed = j > 0 && !(m.fraction[1][g.F(1, ci, j, ck)] > 0);
							std::printf(" %s%.2f", closed ? "|" : "", m.phi[g.C(ci, j, ck)]/g.h);
						}
						std::printf("\n");
					}
				}
			};
		}
		if (c.bowl) {   // the liquid's level set: the layer's top, positive below the bowl
			std::vector<double> p = m.phi;
			const auto& g = m.GridSpec();
			for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int ii = 0; ii < g.n[0]; ++ii) {
				const double x = (ii + 0.5)*h, y = (j + 0.5)*h, z = (k + 0.5)*h;
				double& f = p[g.C(ii, j, k)];
				f = std::max(y - (y0 + c.d), bowlAt(x, z) - y);
			}
			if (!m.SetLevelSet(p, error)) return false;
		}
		if (c.coupled) Relinearise(cfg.material, c.damping);
		return m.ProjectState(error);
	}
	std::vector<double> lastPhi;
	//! m7j diagnostic (M7J_TRACE "t0 t1"): per stage within [t0, t1], the sharp volume change split by
	//! the cells beside a coupled sheet's barrier face (either side), and the three cells that lost most.
	std::vector<double> tracePhi, ring;
	std::map<std::string, std::array<double,3>> traceSums;
	std::map<std::string, std::array<double,4>> traceFlips;   // per stage: cells beside the sheet dried, their dv; wetted, their dv   // beside the sheet, wet-side other, dry-side other
	void Trace(const char* stage) {
		double t0 = 0, t1 = 0;
		if (std::sscanf(std::getenv("M7J_TRACE"), "%lf %lf", &t0, &t1) != 2) return;
		const auto& g = m.GridSpec();
		if (tracePhi.size() == m.phi.size() && m.Time() >= t0 && m.Time() <= t1) {
			std::vector<std::uint8_t> beside(g.Cells(), 0);
			for (const auto& x : m.SheetFaces()) {
				if (!x.Barrier()) continue;
				const auto d = g.FaceDims(x.axis);
				int idx[3] = {int(x.face % std::size_t(d[0])), int((x.face/std::size_t(d[0])) % std::size_t(d[1])), int(x.face/(std::size_t(d[0])*std::size_t(d[1])))};
				beside[g.C(idx[0], idx[1], idx[2])] = 1;
				--idx[x.axis];
				beside[g.C(idx[0], idx[1], idx[2])] = 1;
			}
			// m7j: cells whose WENO stencil along some axis reaches a barrier face (within 3 cells along it), not beside it.
			std::vector<std::uint8_t> reach(g.Cells(), 0);
			for (const auto& x : m.SheetFaces()) {
				if (!x.Barrier()) continue;
				const auto d = g.FaceDims(x.axis);
				int idx[3] = {int(x.face % std::size_t(d[0])), int((x.face/std::size_t(d[0])) % std::size_t(d[1])), int(x.face/(std::size_t(d[0])*std::size_t(d[1])))};
				const int fa = idx[x.axis];
				for (int t = fa - 3; t <= fa + 2; ++t) {
					if (t < 0 || t >= g.n[x.axis]) continue;
					idx[x.axis] = t;
					reach[g.C(idx[0], idx[1], idx[2])] = 1;
				}
			}
			std::array<double,3>& s = traceSums[stage];
			std::vector<std::pair<double, std::size_t>> worst;
			if (ring.empty()) ring.assign(std::size_t(g.n[0]), 0.0);
			for (std::size_t q = 0; q < g.Cells(); ++q) {
				const double a = std::min(1.0, std::max(0.0, 0.5 - tracePhi[q]/h)), b = std::min(1.0, std::max(0.0, 0.5 - m.phi[q]/h));
				const double dv = (b - a)*h*h*h;
				if (dv == 0 || (!topMark.empty() && topMark[q] != 1)) continue;
				s[beside[q] ? 0 : 1] += dv;
				if (!beside[q]) s[2] += reach[q] ? dv : 0.0;
				// m7j: cells beside the sheet that dried (wet to dry at the centre) or wetted in this stage, and their volume change.
				if (beside[q] && (tracePhi[q] < 0) != (m.phi[q] < 0)) {
					std::array<double,4>& t = traceFlips[stage];
					if (m.phi[q] >= 0) { t[0] += 1; t[1] += dv; } else { t[2] += 1; t[3] += dv; }
				}
				if (std::string(stage) == "advected") {
					// The ring: the larger of |i - centre| and |k - centre| (cells).
					const int i = int(q % std::size_t(g.n[0])), k = int(q/(std::size_t(g.n[0])*std::size_t(g.n[1])));
					const double r = std::max(std::fabs(i + 0.5 - 0.5*g.n[0]), std::fabs(k + 0.5 - 0.5*g.n[2]));
					ring[std::min(std::size_t(g.n[0]) - 1, std::size_t(r))] += dv;
				}
				worst.emplace_back(dv, q);
			}
			std::sort(worst.begin(), worst.end());
			if (const char* sl = std::getenv("M7J_SLICE")) {
				// "i stage": the slice x = i, rows j 13..19 (top first), columns k 18..23: phi/h, then v and w (m/s, the cell's lower faces).
				int si = 0; char st[32] = {0};
				if (std::sscanf(sl, "%d %31s", &si, st) == 2 && std::string(st) == stage) {
					std::printf("    SLICE t %.5f %s (phi | v | w)\n", m.Time(), stage);
					for (int j = 19; j >= 13; --j) {
						std::printf("      j %2d", j);
						for (int k = 18; k < g.n[2]; ++k) std::printf(" %s%6.2f", !(m.fraction[1][g.F(1, si, j, k)] > 0) ? "_" : " ", m.phi[g.C(si, j, k)]/h);
						std::printf("  |");
						for (int k = 18; k < g.n[2]; ++k) std::printf(" %6.3f", m.u.u[1][g.F(1, si, j, k)]);
						std::printf("  |");
						for (int k = 18; k < g.n[2]; ++k) std::printf(" %s%6.3f", !(m.fraction[2][g.F(2, si, j, k)] > 0) ? "]" : " ", m.u.u[2][g.F(2, si, j, k)]);
						std::printf("\n");
					}
				}
			}
			int ci = 0, ck = 0;
			if (const char* col = std::getenv("M7J_COL")) if (std::sscanf(col, "%d %d", &ci, &ck) == 2) {
				std::printf("    COL t %.5f %-13s", m.Time(), stage);
				for (int j = 10; j < std::min(g.n[1], 20); ++j) {
					const bool cl = !(m.fraction[1][g.F(1, ci, j, ck)] > 0);
					std::printf(" %s%.3f", cl ? "|" : "", m.phi[g.C(ci, j, ck)]/h);
				}
				std::printf("  uy");
				for (int j = 12; j < std::min(g.n[1], 19); ++j) std::printf(" %.3f", m.u.u[1][g.F(1, ci, j, ck)]);
				std::printf("\n");
				if (const char* cj = std::getenv("M7J_CELLJ")) {
					// One cell's neighbourhood: phi along x and z (offsets -2..2, '|' a closed face before), its face velocities.
					const int j = std::atoi(cj);
					std::printf("      cell (%d %d %d) x:", ci, j, ck);
					for (int o = -2; o <= 2; ++o) { const int i = ci + o; if (i < 0 || i >= g.n[0]) continue;
						std::printf(" %s%.3f", o > -2 && !(m.fraction[0][g.F(0, i, j, ck)] > 0) ? "|" : "", m.phi[g.C(i, j, ck)]/h); }
					std::printf("  z:");
					for (int o = -2; o <= 2; ++o) { const int k = ck + o; if (k < 0 || k >= g.n[2]) continue;
						std::printf(" %s%.3f", o > -2 && !(m.fraction[2][g.F(2, ci, j, k)] > 0) ? "|" : "", m.phi[g.C(ci, j, k)]/h); }
					std::printf("  u %.4f %.4f  v %.4f %.4f  w %.4f %.4f\n", m.u.u[0][g.F(0, ci, j, ck)], m.u.u[0][g.F(0, ci + 1, j, ck)],
						m.u.u[1][g.F(1, ci, j, ck)], m.u.u[1][g.F(1, ci, j + 1, ck)], m.u.u[2][g.F(2, ci, j, ck)], m.u.u[2][g.F(2, ci, j, ck + 1)]);
				}
			}
			if (std::getenv("M7J_TRACE_CELLS") && !worst.empty()) {
				std::printf("    TRACE t %.4f %-13s step %+.3e:", m.Time(), stage, 0.0);
				for (std::size_t e = 0; e < std::min<std::size_t>(3, worst.size()); ++e) {
					const std::size_t q = worst[e].second;
					const int i = int(q % std::size_t(g.n[0])), j = int((q/std::size_t(g.n[0])) % std::size_t(g.n[1])), k = int(q/(std::size_t(g.n[0])*std::size_t(g.n[1])));
					std::printf(" (%d %d %d)%s %.3f->%.3f h uy %.3g %.3g col", i, j, k, beside[q] ? "S" : "", tracePhi[q]/h, m.phi[q]/h,
						m.u.u[1][g.F(1, i, j, k)], m.u.u[1][g.F(1, i, j + 1, k)]);
					for (int jj = std::max(0, j - 3); jj < std::min(g.n[1], j + 4); ++jj) {
						const bool cl = jj > 0 && !(m.fraction[1][g.F(1, i, jj, k)] > 0);
						std::printf(" %s%.2f", cl ? "|" : "", m.phi[g.C(i, jj, k)]/h);
					}
					std::printf(";");
				}
				std::printf("\n");
			}
		}
		tracePhi = m.phi;
		if (std::string(stage) == "pinned" && m.Time() >= t0 && m.Time() <= t1) {
			// m7j: the wet cells held as boundary values (a Dirichlet zero at the sheet) that were not at the last
			// step's end, and the reverse (their faces back to walls), with the phi change the switch made.
			const std::vector<std::uint8_t>& fx = m.Barrier().fixed;
			std::vector<std::uint8_t> now(g.Cells(), 0);
			for (std::size_t q = 0; q < fx.size(); ++q) now[q] = fx[q] && m.phi[q] < 0 && (topMark.empty() || topMark[q] == 1);
			if (lastFixed.size() == now.size())
				for (std::size_t q = 0; q < now.size(); ++q) {
					if (now[q] && !lastFixed[q]) { switches[0] += 1; }
					if (!now[q] && lastFixed[q]) { switches[1] += 1; }
					if (now[q]) switches[2] += 1;
				}
			lastFixed = now;
		}
	}
	std::vector<std::uint8_t> lastFixed;
	double switches[3] = {0, 0, 0};   // m7j: wet cells newly held, released, held (cell-steps)
	mutable std::vector<std::uint8_t> topMark;   // m7j: the cells reachable from the top row at the last Sample (1)
	double SharpVolume() const {   // over the top side (the liquid's side of the sheet) once Sample has run
		double v = 0;
		for (std::size_t q = 0; q < m.phi.size(); ++q) if (topMark.empty() || topMark[q] == 1) v += std::min(1.0, std::max(0.0, 0.5 - m.phi[q]/h));
		return v*h*h*h;
	}
	//! m7i diagnostic (M7I_SURFACE): the extracted liquid surface (LiquidModel::Surface) against the sheet:
	//! of the surface vertices over the sheet's footprint within 2 h of it (nearest triangle by the
	//! distance along its normal, oriented up), those below it (on the dry side) and the deepest.
	void SurfaceCheck() const {
		using namespace Liquid;
		MeshFrame fr;
		std::string e;
		if (!m.Surface(fr, e)) return;
		const auto& l = m.sheets[0];
		const double h = m.GridSpec().h;
		std::size_t near = 0, below = 0, deep = 0, tenth = 0;
		double worst = 0;
		for (std::size_t v = 0; 3*v + 2 < fr.positions.size(); ++v) {
			const double p[3] = {F(fr.positions[3*v]), F(fr.positions[3*v + 1]), F(fr.positions[3*v + 2])};
			double best = 1e9;
			for (std::size_t t = 0; t + 2 < l.triangles.size(); t += 3) {
				const double* a = &l.x[3*std::size_t(l.triangles[t])];
				const double* b = &l.x[3*std::size_t(l.triangles[t + 1])];
				const double* c = &l.x[3*std::size_t(l.triangles[t + 2])];
				double e1[3], e2[3], d[3], n[3];
				for (int k = 0; k < 3; ++k) { e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; d[k] = p[k] - a[k]; }
				n[0] = e1[1]*e2[2] - e1[2]*e2[1]; n[1] = e1[2]*e2[0] - e1[0]*e2[2]; n[2] = e1[0]*e2[1] - e1[1]*e2[0];
				const double nn = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
				if (!(nn > 0)) continue;
				const double sgn = n[1] < 0 ? -1.0 : 1.0;
				const double dist = sgn*(d[0]*n[0] + d[1]*n[1] + d[2]*n[2])/nn;
				if (std::fabs(dist) >= std::fabs(best)) continue;
				// Inside the triangle (projected along its normal).
				const double d11 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2], d12 = e1[0]*e2[0] + e1[1]*e2[1] + e1[2]*e2[2], d22 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2];
				const double p1 = d[0]*e1[0] + d[1]*e1[1] + d[2]*e1[2], p2 = d[0]*e2[0] + d[1]*e2[1] + d[2]*e2[2];
				const double det = d11*d22 - d12*d12, u = (d22*p1 - d12*p2)/det, w = (d11*p2 - d12*p1)/det;
				if (u < 0 || w < 0 || u + w > 1) continue;
				best = dist;
			}
			if (!(std::fabs(best) < 2*h)) continue;
			++near;
			if (best < 0) { ++below; worst = std::min(worst, best); if (best < -0.1*h) ++tenth; if (best < -0.5*h) ++deep; }
		}
		std::printf("    surface: %zu vertices within 2 h over the sheet, %zu below it (%zu by > 0.1 h, %zu by > 0.5 h), deepest %.3g h\n",
			near, below, tenth, deep, worst/h);
	}
	double DeepVolume() const {
		const auto& g = m.GridSpec();
		const double eps = 1.5*g.h, cell = g.h*g.h*g.h;
		double s = 0;
		for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) {
			if ((j + 0.5)*g.h >= y0 - 0.055) break;
			for (int i = 0; i < g.n[0]; ++i) {
				const double p = m.phi[g.C(i, j, k)];
				s += p < -eps ? 1.0 : p > eps ? 0.0 : 1 - 0.5*(1 + p/eps + std::sin(Pi*p/eps)/Pi);
			}
		}
		return s*cell;
	}
	//! m7g diagnostic: marker particles below y0 - 0.055 m (negative, positive) and the least phi/h there.
	void DeepParticles(std::size_t& neg, std::size_t& pos, double& least) const {
		neg = pos = 0;
		for (const auto& p : m.particles.Particles()) if (p.x[1] < y0 - 0.055) { if (p.sign < 0) ++neg; else ++pos; }
		const auto& g = m.GridSpec();
		least = 1e9;
		for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) {
			if ((j + 0.5)*g.h >= y0 - 0.055) break;
			for (int i = 0; i < g.n[0]; ++i) least = std::min(least, m.phi[g.C(i, j, k)]/g.h);
		}
	}
	Cloth::Material material;
	bool damped = false;
	void Relinearise(const Cloth::Material& mat, bool damping) {
		material = mat; damped = damping;
		Relinearise();
	}
	void Relinearise() {
		using namespace Liquid;
		LiquidSheet& l = m.sheets[0];
		cloth.x = l.dyn.Active() ? l.dyn.xe : l.x;
		cloth.v = l.v;
		l.x = cloth.x;
		std::vector<double> f;
		Cloth::BlockMatrix K;
		cloth.Mechanics(f, K);
		SheetDynamics& d = l.dyn;
		d.mass = cloth.GetSheet().mass;
		d.fixed.assign(d.mass.size(), 0);
		for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) d.fixed[q] = cloth.Fixed(q) || (c.freeze >= 0 && m.Time() >= c.freeze - 1e-9);
		if (c.freeze >= 0 && m.Time() >= c.freeze - 1e-9) { std::fill(l.v.begin(), l.v.end(), 0.0); cloth.v = l.v; }   // m7j: the sheet held where it is
		d.force = f;
		d.stiffness = K;
		d.dampingMass = damped ? material.dampingMass : 0.0;
		d.dampingStiffness = damped ? material.dampingStiffness : 0.0;
		m.UpdateSheetGeometry(true);
		m.ResetSheetDynamics();
	}
	bool Macro() {
		const auto after = [&](double, const Liquid::StepStats& st) {
			// The volume the correction added in the step (phi += shift removes shift x area).
			added -= st.volumeShift*Liquid::InterfaceArea(m.GridSpec(), m.phi, nullptr, m.ThreadPool());
			kept += st.sheetCellsKept;
			if (std::getenv("M7F_SHIFT")) std::printf("    step %.4f shift %.3g kept %zu removed %zu reinit %d\n", m.Time(), st.volumeShift, st.sheetCellsKept, st.sheetParticlesRemoved, int(st.reinitialised));
		};
		if (!m.Run(c.macro, c.macro, after, error)) return false;
		if (c.coupled) Relinearise();
		return true;
	}
	CupSample Sample() const {
		const auto& g = m.GridSpec();
		CupSample s;
		s.t = m.Time();
		s.added = added;
		s.smoothed = m.Volume();
		std::vector<std::uint8_t> mark(g.Cells(), 0);   // 1 top, 2 bottom, 3 both
		for (int side = 0; side < 2; ++side) {
			const std::uint8_t bit = std::uint8_t(side ? 2 : 1);
			std::vector<std::size_t> stack;
			const int j0 = side ? 0 : g.n[1] - 1;
			for (int k = 0; k < g.n[2]; ++k) for (int i = 0; i < g.n[0]; ++i) { const std::size_t q = g.C(i, j0, k); mark[q] |= bit; stack.push_back(q); }
			while (!stack.empty()) {
				const std::size_t q = stack.back(); stack.pop_back();
				const int idx[3] = {int(q % std::size_t(g.n[0])), int((q/std::size_t(g.n[0])) % std::size_t(g.n[1])), int(q/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
				for (int a = 0; a < 3; ++a) for (int dir = -1; dir <= 1; dir += 2) {
					int nb[3] = {idx[0], idx[1], idx[2]};
					nb[a] += dir;
					if (nb[a] < 0 || nb[a] >= g.n[a]) continue;
					int f[3] = {idx[0], idx[1], idx[2]};
					if (dir > 0) ++f[a];
					if (!(m.fraction[std::size_t(a)][g.F(a, f[0], f[1], f[2])] > 0)) continue;
					const std::size_t r = g.C(nb[0], nb[1], nb[2]);
					if (!(mark[r] & bit)) { mark[r] |= bit; stack.push_back(r); }
				}
			}
		}
		const double cell = g.h*g.h*g.h;
		// The cells beside the sheet's closed faces: on the dry side their part-full count is the
		// half cell between the centre and the sheet that the liquid above fills.
		std::vector<std::uint8_t> besideSheet(g.Cells(), 0);
		for (const auto& x : m.SheetFaces()) {
			const auto d = g.FaceDims(x.axis);
			int idx[3] = {int(x.face % std::size_t(d[0])), int((x.face/std::size_t(d[0])) % std::size_t(d[1])), int(x.face/(std::size_t(d[0])*std::size_t(d[1])))};
			besideSheet[g.C(idx[0], idx[1], idx[2])] = 1;
			--idx[x.axis];
			besideSheet[g.C(idx[0], idx[1], idx[2])] = 1;
		}
		if (std::getenv("M7F_DUMP") && m.Time() > std::atof(std::getenv("M7F_DUMP"))) {
			int shown = 0;
			for (std::size_t q = 0; q < g.Cells() && shown < 12; ++q) {
				if (mark[q] != 2 || !(m.phi[q] < 0.5*g.h) || besideSheet[q]) continue;
				const int i = int(q % std::size_t(g.n[0])), j = int((q/std::size_t(g.n[0])) % std::size_t(g.n[1])), k = int(q/(std::size_t(g.n[0])*std::size_t(g.n[1])));
				std::printf("      dry cell (%d %d %d) phi %.3g h; column:", i, j, k, m.phi[q]/g.h);
				for (int jj = std::max(0, j - 2); jj < std::min(g.n[1], j + 5); ++jj) {
					const bool cl = jj > 0 && !(m.fraction[1][g.F(1, i, jj, k)] > 0);
					std::printf(" %s%.2f", cl ? "|" : "", m.phi[g.C(i, jj, k)]/g.h);
				}
				std::printf("; x nbrs %.2f %.2f z nbrs %.2f %.2f\n", i > 0 ? m.phi[g.C(i - 1, j, k)]/g.h : 99.0, i + 1 < g.n[0] ? m.phi[g.C(i + 1, j, k)]/g.h : 99.0,
					k > 0 ? m.phi[g.C(i, j, k - 1)]/g.h : 99.0, k + 1 < g.n[2] ? m.phi[g.C(i, j, k + 1)]/g.h : 99.0);
				++shown;
			}
		}
		topMark = mark;
		for (std::size_t q = 0; q < g.Cells(); ++q) {
			const double v = std::min(1.0, std::max(0.0, 0.5 - m.phi[q]/g.h))*cell;
			if (mark[q] == 3) s.breached = true;
			if (mark[q] == 1) s.top += v;
			else if (mark[q] & 2) {
				s.bottom += v;
				if (m.phi[q] < 0) s.bottomWet += cell;
				if (!besideSheet[q]) s.beyond += v;
			} else s.other += v;
		}
		const auto& l = m.sheets[0];
		const std::size_t mid = std::size_t(c.vertices/2)*std::size_t(c.vertices) + std::size_t(c.vertices/2);
		s.sag = y0 - l.x[3*mid + 1];
		if (std::getenv("M7F_SURFACE")) {
			// The free surface (the topmost zero of phi) along columns at a quarter and the middle of the tank.
			for (int q : {g.n[0]/4, g.n[0]/2}) {
				double ys = -1;
				for (int j = g.n[1] - 1; j > 0; --j) {
					const double a = m.phi[g.C(q, j, g.n[2]/2)], b = m.phi[g.C(q, j - 1, g.n[2]/2)];
					if (a >= 0 && b < 0) { ys = (j - 0.5 + a/(a - b))*g.h; break; }
				}
				std::printf("    column %d: free surface %.5f", q, ys);
			}
			std::printf("  (sheet mid %.5f)\n", l.x[3*mid + 1]);
		}
		for (std::size_t q = 0; q < l.Vertices(); ++q)
			s.vmax = std::max(s.vmax, std::sqrt(l.v[3*q]*l.v[3*q] + l.v[3*q + 1]*l.v[3*q + 1] + l.v[3*q + 2]*l.v[3*q + 2]));
		return s;
	}
};

// Runs a cup; prints a line every `every` s.  Returns the samples (one per macro step).
std::vector<CupSample> RunCup(const CupCase& c, double every, bool& ok, std::string& error)
{
	std::vector<CupSample> out;
	auto rig = std::make_unique<CupRig>();
	ok = rig->Build(c);
	if (!ok) { error = rig->error; return out; }
	out.push_back(rig->Sample());
	const double V0 = out[0].top;
	const double w0 = Seconds();
	while (rig->m.Time() < c.T - 1e-9) {
		if (!rig->Macro()) { ok = false; error = rig->error; return out; }
		out.push_back(rig->Sample());
		rig->lastStageVolume = std::getenv("M7G_DEEP") ? rig->DeepVolume() : std::getenv("M7J_SHARP") ? rig->SharpVolume() : rig->m.Volume();   // macro boundaries (the relinearisation's sweep) are not a stage
		const CupSample& s = out.back();
		if (every > 0 && std::fabs(s.t/every - std::round(s.t/every)) < 1e-6)
			std::printf("  t %.3f sag %.4f m, max|v| %.3g; liquid above %.6g, below %.4g (%.2e of V0; beyond the sheet's cells %.2e, wet cells %.2e), elsewhere %.3g; smoothed %.6g; correction added %.3g (%.2e of V0)%s (%.0f s)\n",
				s.t, s.sag, s.vmax, s.top, s.bottom, s.bottom/V0, s.beyond/V0, s.bottomWet/V0, s.other, s.smoothed, s.added, s.added/V0, s.breached ? " BREACHED" : "", Seconds() - w0);
		if (every > 0 && std::fabs(s.t/every - std::round(s.t/every)) < 1e-6 && std::getenv("M7I_SURFACE")) rig->SurfaceCheck();
		if (every > 0 && std::fabs(s.t/every - std::round(s.t/every)) < 1e-6 && std::getenv("M7J_VMAX")) {
			// m7j: the fastest valid liquid face (m/s) and where.
			const auto& g = rig->m.GridSpec();
			double best = 0;
			int at[4] = {0, 0, 0, 0};
			for (int a = 0; a < 3; ++a) {
				const auto d = g.FaceDims(a);
				for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
					const std::size_t f = g.F(a, i, j, k);
					if (rig->m.valid[std::size_t(a)][f] && std::fabs(rig->m.u.u[std::size_t(a)][f]) > best) { best = std::fabs(rig->m.u.u[std::size_t(a)][f]); at[0] = a; at[1] = i; at[2] = j; at[3] = k; }
				}
			}
			std::printf("    liquid max |u| %.4f m/s, axis %d face (%d %d %d)\n", best, at[0], at[1], at[2], at[3]);
			// The divergence of u (the step-start field; face values as the level set's cell velocity reads them)
			// in the wet cells, h div / max|u|: largest and summed, beside a sheet face and elsewhere.
			std::vector<std::uint8_t> beside(g.Cells(), 0);
			for (const auto& x : rig->m.SheetFaces()) {
				const auto d = g.FaceDims(x.axis);
				int idx[3] = {int(x.face % std::size_t(d[0])), int((x.face/std::size_t(d[0])) % std::size_t(d[1])), int(x.face/(std::size_t(d[0])*std::size_t(d[1])))};
				beside[g.C(idx[0], idx[1], idx[2])] = 1;
				--idx[x.axis];
				beside[g.C(idx[0], idx[1], idx[2])] = 1;
			}
			double worst[2] = {0, 0}, sum[2] = {0, 0}, net[2] = {0, 0};
			for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
				const std::size_t q = g.C(i, j, k);
				if (!(rig->m.phi[q] < 0) || (!rig->topMark.empty() && rig->topMark[q] != 1)) continue;
				const auto& U = rig->m.u.u;
				const double dv = U[0][g.F(0, i + 1, j, k)] - U[0][g.F(0, i, j, k)] + U[1][g.F(1, i, j + 1, k)] - U[1][g.F(1, i, j, k)] + U[2][g.F(2, i, j, k + 1)] - U[2][g.F(2, i, j, k)];
				const int b = beside[q] ? 0 : 1;
				worst[b] = std::max(worst[b], std::fabs(dv)/best); sum[b] += std::fabs(dv)/best; net[b] += dv*g.h*g.h;
			}
			std::printf("    h div u / max|u| in wet cells: beside the sheet largest %.3g, sum %.3g, net flux %+.3g m^3/s; elsewhere largest %.3g, sum %.3g, net %+.3g m^3/s\n",
				worst[0], sum[0], net[0], worst[1], sum[1], net[1]);
		}
		if (every > 0 && std::fabs(s.t/every - std::round(s.t/every)) < 1e-6 && std::getenv("M7G_DEEP")) {
			std::size_t neg = 0, pos = 0;
			double least = 0;
			rig->DeepParticles(neg, pos, least);
			std::printf("    deep: volume %.3e of V0, particles %zu negative %zu positive, least phi %.3f h\n", rig->DeepVolume()/V0, neg, pos, least);
		}
	}
	for (const auto& e : rig->stageSums) std::printf("  stage %-14s %+.3e m^3 (%+.2e of V0)\n", e.first.c_str(), e.second, e.second/V0);
	for (const auto& e : rig->traceSums) std::printf("  trace %-14s beside the sheet %+.2e, elsewhere %+.2e (within a stencil of the sheet %+.2e) of V0\n", e.first.c_str(), e.second[0]/V0, e.second[1]/V0, e.second[2]/V0);
	for (const auto& e : rig->traceFlips) std::printf("  flips %-14s beside the sheet: dried %.0f (%+.2e of V0), wetted %.0f (%+.2e of V0)\n", e.first.c_str(), e.second[0], e.second[1]/V0, e.second[2], e.second[3]/V0);
	if (rig->switches[2] > 0) std::printf("  wet boundary cells (held at the sheet's distance): %.0f cell-steps held, %.0f switches to held, %.0f to released\n", rig->switches[2], rig->switches[0], rig->switches[1]);
	if (!rig->ring.empty()) { std::printf("  advection by ring:"); for (std::size_t r = 0; r < rig->ring.size()/2; ++r) std::printf(" %+.3f", rig->ring[r]/V0); std::printf("\n"); }
	if (every > 0) std::printf("  through-sheet measure: cells wet across the sheet %llu (%.2e of V0, held), wet without a wet neighbour %llu\n",
		static_cast<unsigned long long>(rig->m.sheetCrossedCells), rig->m.sheetCrossedVolume/V0, static_cast<unsigned long long>(rig->m.sheetUnfedCells));
	return out;
}

// m7j: the level set's advection alone beside a held sheet: the bowl of --cup (bowl 1) with a pool
// at its centre (its waterline a circle inside the tank, depth `depth` below the rim: negative), and a
// rigid rotation about the vertical axis through the bowl's centre (omega rad/s), tangent to the bowl
// and to the level surface: the exact solution does not change.  The same pool with the bowl not
// represented in the level set (phi = y - y_s everywhere, the region below the bowl masked out of
// the count) is the rigid wall's analogue.  Prints the sharp volume above the sheet against its start.
void SpinProbe(int argc, char** argv)
{
	CupCase c;
	c.bowl = c.frozen = true;
	c.d = -0.005;
	c.particles = false;
	double omega = 2, T = 2;
	bool barrier = true;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "cells") c.cells = std::atoi(v.c_str());
		else if (k == "depth") c.d = std::atof(v.c_str());
		else if (k == "omega") omega = std::atof(v.c_str());
		else if (k == "T") T = std::atof(v.c_str());
		else if (k == "barrier") barrier = std::atoi(v.c_str()) != 0;
	}
	auto rig = std::make_unique<CupRig>();
	if (!rig->Build(c)) { std::printf("failed: %s\n", rig->error.c_str()); return; }
	rig->Sample();   // the top side's cells
	using namespace Liquid;
	const Grid& g = rig->m.GridSpec();
	const double h = g.h;
	std::vector<double> phi = rig->m.phi;
	const double bowlR = (0.25*c.L*c.L + 0.03*0.03)/(2*0.03), bowlY = rig->y0 - 0.03 + bowlR;
	std::vector<std::uint8_t> inside(g.Cells(), 0);   // cell centres above the bowl
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const double x = (i + 0.5)*h - 0.5*c.L, y = (j + 0.5)*h, z = (k + 0.5)*h - 0.5*c.L;
		const double yb = bowlY - std::sqrt(std::max(0.0, bowlR*bowlR - x*x - z*z));
		inside[g.C(i, j, k)] = y > yb;
		if (!barrier) phi[g.C(i, j, k)] = y - (rig->y0 + c.d);
	}
	auto volume = [&]() {
		double v = 0;
		for (std::size_t q = 0; q < phi.size(); ++q)
			if (barrier ? rig->topMark[q] == 1 : inside[q] != 0) v += std::min(1.0, std::max(0.0, 0.5 - phi[q]/h));
		return v*h*h*h;
	};
	std::array<std::vector<double>,3> vel;
	for (auto& a : vel) a.assign(g.Cells(), 0.0);
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const double x = (i + 0.5)*h - 0.5*c.L, z = (k + 0.5)*h - 0.5*c.L;
		vel[0][g.C(i, j, k)] = -omega*z;
		vel[2][g.C(i, j, k)] = omega*x;
	}
	const double V0 = volume(), dt = 0.5*h/(omega*0.15);
	const int n = int(std::ceil(T/dt));
	for (int s = 1; s <= n; ++s) {
		AdvectLevelSet(g, phi, [&](double, std::array<std::vector<double>,3>& v) { v = vel; }, 0.0, T/n, rig->m.ThreadPool(),
			barrier ? &rig->m.Barrier() : nullptr);
		if (s % std::max(1, n/4) == 0) std::printf("  t %.3f volume %+.3e of V0\n", s*T/n, volume()/V0 - 1);
	}
	std::printf("spin cells %d depth %g omega %g T %g barrier %d: %d steps, volume %+.3e of V0\n", c.cells, c.d, omega, T, int(barrier), n, volume()/V0 - 1);
}

// m7j: cloth.shell alone in air (no liquid): the flap of --probe (0.36 x 0.45 m, 12 x 12 vertices,
// pinned along its top edge, tilted 17 degrees) thrown with a whip (normal velocity V (s / L)^2, s the
// distance from the pinned edge), integrated by backward Euler at the liquid's step size, by the
// nonlinear model (`linear 0`) or by the coupled step's corotational linear model about each macro
// step's start (`linear 1`, the model the cloth and the liquid's copy share inside a macro step).
// Prints the largest speed, the largest change of a vertex's velocity in one step and the largest
// rotation lag (linear only), and the largest edge strain against the rest lengths.
void AirProbe(int argc, char** argv)
{
	const auto Sq = [](double x) { return x*x; };
	double sigma = 0.05, V = 3, T = 0.5, macro = 1.0/30, dt = 1.0/1440;
	bool linear = true;
	int vertices = 12;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "density") sigma = std::atof(v.c_str());
		else if (k == "V") V = std::atof(v.c_str());
		else if (k == "T") T = std::atof(v.c_str());
		else if (k == "macro") macro = 1/std::atof(v.c_str());
		else if (k == "dt") dt = 1/std::atof(v.c_str());
		else if (k == "linear") linear = std::atoi(v.c_str()) != 0;
		else if (k == "vertices") vertices = std::atoi(v.c_str());
	}
	using namespace Cloth;
	const double tilt = 17*Pi/180;
	SheetSpec spec;
	spec.sizeU = 0.36; spec.sizeV = 0.45;
	spec.verticesU = spec.verticesV = std::uint32_t(vertices);
	spec.origin = {{0.6123, 0.55, 0.02}}; spec.axisU = {{0, 0, 1}}; spec.axisV = {{std::sin(tilt), -std::cos(tilt), 0}};
	ModelConfig cfg;
	FindMaterial("cotton-plain-weave", cfg.material);
	cfg.material.density = sigma;
	cfg.gravity = {{0, -9.81, 0}};
	Sheet sheet;
	std::string e;
	if (!BuildSheet(spec, cfg.material, sheet, e)) { std::printf("failed: %s\n", e.c_str()); return; }
	PinGroup top;
	for (std::uint32_t i = 0; i < spec.verticesU; ++i) top.vertices.push_back(sheet.Index(i, 0));
	cfg.pins = {top};
	ClothSystem cloth;
	if (!cloth.Initialize(sheet, cfg, e)) { std::printf("failed: %s\n", e.c_str()); return; }
	cloth.Reset(0);
	const std::vector<double> rest = cloth.x;
	const double n[3] = {std::cos(tilt), std::sin(tilt), 0};   // the sheet's normal (axis_u x axis_v up to sign)
	for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) {
		if (cloth.Fixed(q)) continue;
		const double s = std::sqrt(Sq(cloth.x[3*q] - 0.6123) + Sq(cloth.x[3*q + 1] - 0.55))/0.45;
		for (int a = 0; a < 3; ++a) cloth.v[3*q + std::size_t(a)] = V*s*s*n[a];
	}
	NewtonOptions o;
	StepStats st;
	double vmax = 0, jump = 0, lag = 0, strain = 0, jumpAt = 0;
	const int perMacro = int(std::lround(macro/dt));
	const int macros = int(std::lround(T/macro));
	for (int k = 0; k < macros; ++k) {
		if (linear) cloth.BeginLinear();
		for (int s = 0; s < perMacro; ++s) {
			const std::vector<double> v0 = cloth.v;
			if (!cloth.Step(dt, Scheme::BackwardEuler, o, st, e)) { std::printf("step failed: %s\n", e.c_str()); return; }
			if (linear) lag = std::max(lag, cloth.RotationLag());
			for (std::uint32_t q = 0; q < cloth.Vertices(); ++q) {
				double d2 = 0, s2 = 0;
				for (int a = 0; a < 3; ++a) { d2 += Sq(cloth.v[3*q + std::size_t(a)] - v0[3*q + std::size_t(a)]); s2 += Sq(cloth.v[3*q + std::size_t(a)]); }
				vmax = std::max(vmax, std::sqrt(s2));
				if (std::sqrt(d2) > jump) { jump = std::sqrt(d2); jumpAt = cloth.time; }
			}
			const auto& tri = cloth.GetSheet().triangles;
			for (const auto& t : tri) for (int m = 0; m < 3; ++m) {
				const std::uint32_t a0 = t.v[m], b0 = t.v[(m + 1) % 3];
				double l = 0, l0 = 0;
				for (int a = 0; a < 3; ++a) { l += Sq(cloth.x[3*b0 + std::size_t(a)] - cloth.x[3*a0 + std::size_t(a)]); l0 += Sq(rest[3*b0 + std::size_t(a)] - rest[3*a0 + std::size_t(a)]); }
				strain = std::max(strain, std::fabs(std::sqrt(l/l0) - 1));
			}
		}
		if (linear) cloth.EndLinear();
	}
	std::printf("air density %g V %g dt 1/%g macro 1/%g linear %d: max |v| %.3g m/s, largest one-step |dv| %.3g m/s (t %.4f), rotation lag %.3g rad, edge strain %.3g\n",
		sigma, V, 1/dt, 1/macro, int(linear), vmax, jump, jumpAt, lag, strain);
}

void CupProbe(int argc, char** argv)
{
	CupCase c;
	double every = 0.25;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "cells") c.cells = std::atoi(v.c_str());
		else if (k == "density") c.sigma = std::atof(v.c_str());
		else if (k == "T") c.T = std::atof(v.c_str());
		else if (k == "macro") c.macro = 1/std::atof(v.c_str());
		else if (k == "E") c.E = std::atof(v.c_str());
		else if (k == "vertices") c.vertices = std::atoi(v.c_str());
		else if (k == "depth") c.d = std::atof(v.c_str());
		else if (k == "volume") c.volume = std::atoi(v.c_str()) != 0;
		else if (k == "particles") c.particles = std::atoi(v.c_str()) != 0;
		else if (k == "damping") c.damping = std::atoi(v.c_str()) != 0;
		else if (k == "reinit") c.reinit = std::atoi(v.c_str()) != 0;
		else if (k == "every") every = std::atof(v.c_str());
		else if (k == "frozen") c.frozen = std::atoi(v.c_str()) != 0;
		else if (k == "offset") c.offset = std::atof(v.c_str());
		else if (k == "freeze") c.freeze = std::atof(v.c_str());
		else if (k == "u0") c.u0 = std::atof(v.c_str());
		else if (k == "bowl") { c.bowl = std::atoi(v.c_str()) != 0; c.frozen = c.frozen || c.bowl; }
		else if (k == "coupled") c.coupled = std::atoi(v.c_str()) != 0;
	}
	bool ok = false;
	std::string e;
	const auto s = RunCup(c, every, ok, e);
	if (!ok) { std::printf("failed: %s\n", e.c_str()); return; }
	double worst = 0, worstBeyond = 0, worstWet = 0;
	bool breached = false;
	for (const CupSample& x : s) {
		worst = std::max(worst, x.bottom); worstBeyond = std::max(worstBeyond, x.beyond); worstWet = std::max(worstWet, x.bottomWet);
		breached = breached || x.breached;
	}
	const double V0 = s[0].top;
	std::printf("cup cells %d sigma %g E %g depth %g macro 1/%g T %g: V0 %.6g m^3; below the sheet: max %.2e of V0 (beyond the sheet's cells %.2e, wet cells %.2e); "
		"final: above %+.2e, above + below %+.2e, smoothed %+.2e of V0; correction added %+.2e of V0%s\n",
		c.cells, c.sigma, c.E, c.d, 1/c.macro, c.T, V0, worst/V0, worstBeyond/V0, worstWet/V0, s.back().top/V0 - 1, (s.back().top + s.back().bottom)/V0 - 1,
		s.back().smoothed/s[0].smoothed - 1, s.back().added/V0, breached ? ", BREACHED" : ", barrier intact");
}

// ---------------------------------------------------------------- m7g: a thin layer on a rigid floor
// The control for the layer on a sheet: the cup's tank (L x H x L) with no sheet and a layer of
// depth d on its floor, set sloshing by a uniform initial velocity u0 along x, the same numerics.
// Reports the sharp volume sum clamp(1/2 - phi/h, 0, 1) h^3 against its start and the volume the
// correction added (sum -shift x area).
void LayerProbe(int argc, char** argv)
{
	using namespace Liquid;
	int cells = 24;
	double L = 0.3, H = 0.3, d = 0.03, u0 = 0.1, T = 2, bowl = 0;
	bool volume = true, particles = true;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "cells") cells = std::atoi(v.c_str());
		else if (k == "depth") d = std::atof(v.c_str());
		else if (k == "u0") u0 = std::atof(v.c_str());
		else if (k == "T") T = std::atof(v.c_str());
		else if (k == "volume") volume = std::atoi(v.c_str()) != 0;
		else if (k == "particles") particles = std::atoi(v.c_str()) != 0;
		else if (k == "bowl") bowl = std::atof(v.c_str());
	}
	const double h = L/cells;
	LiquidSetup s;
	s.grid.n = {{cells, int(std::lround(H/h)), cells}};
	s.grid.h = h;
	Shape col;
	col.type = Shape::Type::Box;
	col.lo = {{-1, -1, -1}};
	col.hi = {{2, d, 2}};
	s.liquid.push_back(col);
	if (bowl > 0) {
		// m7j: a rigid spherical bowl (sagitta 0.03 m over the half width, rim at 0.2 m), the layer d above its rim.
		const double R = (0.25*L*L + 0.03*0.03)/(2*0.03);
		Shape b;
		b.type = Shape::Type::Sphere;
		b.point = {{0.5*L, 0.2 - 0.03 + R, 0.5*L}};
		b.radius = R;
		b.invert = true;
		s.solids.push_back(b);
		s.liquid[0].hi[1] = 0.2 + d;
	}
	s.initialVelocity = {{u0, 0, 0}};
	SolverOptions so;
	so.volumeCorrection = volume;
	so.particles = particles;
	if (std::getenv("M7J_MINFACE")) so.minFaceFraction = std::atof(std::getenv("M7J_MINFACE"));   // m7j: a staircase-like solid
	if (std::getenv("M7J_NOREINIT")) so.reinit = ReinitScheme::None;
	LiquidModel m;
	std::string e;
	if (!m.Initialize(s, so, 8, e)) { std::printf("failed: %s\n", e.c_str()); return; }
	auto sharp = [&]() {   // m7j: weighted by the open cell fraction (with a bowl)
		double v = 0;
		for (std::size_t q = 0; q < m.phi.size(); ++q) v += std::min(1.0, std::max(0.0, 0.5 - m.phi[q]/h))*(m.openCells.empty() ? 1.0 : m.openCells[q]);
		return v*h*h*h; };
	const double V0 = sharp(), S0 = m.Volume();
	double added = 0, worst = 0;
	const double w0 = Seconds();
	for (int q = 1; q <= int(std::lround(T/0.25)); ++q) {
		if (!m.Run(0.25, 1.0/120, [&](double, const StepStats& st) { added -= st.volumeShift*InterfaceArea(m.GridSpec(), m.phi, nullptr, m.ThreadPool()); }, e)) {
			std::printf("failed: %s\n", e.c_str()); return; }
		worst = std::max(worst, std::fabs(sharp()/V0 - 1));
		std::printf("  t %.2f sharp %+.2e smoothed %+.2e of V0, correction added %+.2e of V0, max|u| %.3f (%.0f s)\n", m.Time(), sharp()/V0 - 1, m.Volume()/S0 - 1, added/V0, m.MaxSpeed(), Seconds() - w0);
	}
	std::printf("layer cells %d depth %g (%.1f cells) u0 %g T %g volume %d: sharp %+.2e (worst %.2e), smoothed %+.2e, correction added %+.2e of V0\n",
		cells, d, d/h, u0, T, int(volume), sharp()/V0 - 1, worst, m.Volume()/S0 - 1, added/V0);
}

// ---------------------------------------------------------------- m7f: rotation within a macro step

// A free cotton sheet (0.3 m, 13 x 13, no gravity, no damping) spinning rigidly at omega about
// an axis in its plane (flapping) or along its normal, integrated by backward Euler in
// `substeps` steps per macro step H with the coupled step's linear model (BeginLinear at each
// macro start, as cloth.shell does with its partner's steps) or with the cloth's own nonlinear
// model.  The largest edge strain |l / l0 - 1| (rows and columns) over `T` against (omega H)^2 / 2,
// the stretch of straight-line motion over the step (derived: a point at r moving tangentially
// by omega r H ends at r sqrt(1 + (omega H)^2)).
double MaxEdgeStrain(const Cloth::ClothSystem& cloth, std::uint32_t n)
{
	const Cloth::Sheet& s = cloth.GetSheet();
	double worst = 0;
	for (std::uint32_t j = 0; j < n; ++j) for (std::uint32_t i = 0; i < n; ++i)
		for (int d = 0; d < 2; ++d) {
			if ((d == 0 && i + 1 >= n) || (d == 1 && j + 1 >= n)) continue;
			const std::uint32_t a = s.Index(i, j), b = d == 0 ? s.Index(i + 1, j) : s.Index(i, j + 1);
			double l = 0;
			for (int k = 0; k < 3; ++k) { const double e = cloth.x[3*b + std::uint32_t(k)] - cloth.x[3*a + std::uint32_t(k)]; l += e*e; }
			worst = std::max(worst, std::fabs(std::sqrt(l)/(d == 0 ? s.hu : s.hv) - 1));
		}
	return worst;
}

// model: 0 the cloth's nonlinear model, 1 m7e's linear model about the macro step's start, 2 m7h's corotational one.
double RotatingSheet(double omega, double H, int substeps, int model, bool inPlaneAxis, double T)
{
	using namespace Cloth;
	SheetSpec spec;
	spec.sizeU = spec.sizeV = 0.3;
	spec.verticesU = spec.verticesV = 13;
	ModelConfig cfg;
	FindMaterial("cotton-plain-weave", cfg.material);
	cfg.gravity = {{0, 0, 0}};
	cfg.damping = false;
	Sheet sheet;
	std::string e;
	BuildSheet(spec, cfg.material, sheet, e);
	ClothSystem cloth;
	cloth.Initialize(sheet, cfg, e);
	cloth.Reset(0);
	cloth.SetCorotated(model == 2);
	const bool linear = model != 0;
	const std::uint32_t n = cloth.Vertices();
	std::array<double,3> centre{{0, 0, 0}};
	for (std::uint32_t q = 0; q < n; ++q) for (int k = 0; k < 3; ++k) centre[std::size_t(k)] += cloth.x[3*q + std::uint32_t(k)]/n;
	// The sheet lies in the xz plane (axis_u x, axis_v -z): an in-plane axis is x, the normal y.
	const std::array<double,3> w = inPlaneAxis ? std::array<double,3>{{omega, 0, 0}} : std::array<double,3>{{0, omega, 0}};
	for (std::uint32_t q = 0; q < n; ++q) {
		const double r[3] = {cloth.x[3*q] - centre[0], cloth.x[3*q + 1] - centre[1], cloth.x[3*q + 2] - centre[2]};
		cloth.v[3*q] = w[1]*r[2] - w[2]*r[1];
		cloth.v[3*q + 1] = w[2]*r[0] - w[0]*r[2];
		cloth.v[3*q + 2] = w[0]*r[1] - w[1]*r[0];
	}
	NewtonOptions o;
	double worst = 0;
	for (double t = 0; t < T - 1e-9; t += H) {
		if (linear) cloth.BeginLinear();
		for (int k = 0; k < substeps; ++k) {
			Cloth::StepStats st;
			if (!cloth.Step(H/substeps, Scheme::BackwardEuler, o, st, e)) { std::printf("step failed: %s\n", e.c_str()); return NAN; }
			worst = std::max(worst, MaxEdgeStrain(cloth, 13));
		}
		cloth.EndLinear();
	}
	return worst;
}

void RotateProbe(int, char**)
{
	std::printf("rotation within a macro step: max edge strain over 0.5 s (backward Euler, 4 steps per macro step)\n");
	for (int axis = 0; axis < 2; ++axis)
		for (double H : {1.0/120, 1.0/30})
			for (double omega : {1.0, 3.0, 10.0, 30.0}) {
				const double lin = RotatingSheet(omega, H, 4, 1, axis == 0, 0.5), non = RotatingSheet(omega, H, 4, 0, axis == 0, 0.5),
					cor = RotatingSheet(omega, H, 4, 2, axis == 0, 0.5);
				std::printf("  %s omega %4.0f rad/s, H 1/%3.0f s (omega H %.3f): linear per macro step %.2e, corotational %.2e, nonlinear %.2e, "
					"(omega H)^2/2 %.2e\n", axis == 0 ? "in-plane axis" : "normal axis  ", omega, 1/H, omega*H, lin, cor, non, 0.5*omega*H*omega*H);
			}
}

double Frequency(const std::vector<double>& t, const std::vector<double>& y)
{
	double mean = 0;
	for (double x : y) mean += x/double(y.size());
	std::vector<double> zc;
	for (std::size_t j = 1; j < y.size(); ++j)
		if ((y[j - 1] - mean > 0) != (y[j] - mean > 0)) zc.push_back(t[j - 1] + (t[j] - t[j - 1])*(y[j - 1] - mean)/(y[j - 1] - y[j]));
	return zc.size() > 2 ? Pi*double(zc.size() - 1)/(zc.back() - zc.front()) : NAN;
}


// B: the light flap (cotton weight) is stable in the implicit coupling and diverges in
// the staggered one (the M7d exchange) on the same case.
void Stability()
{
	std::printf("B: stability\n");
	FlapCase c;
	c.cells = 24; c.T = 1; c.density = 0.2;
	const FlapResult a = RunFlap(c);
	std::printf("  implicit, 0.2 kg/m^2, 24 cells, 1/30 s: %s, max |v| %.3g m/s (submerged %.3g), roughness %.3g (%.1f s)\n", a.ok ? "stable" : "BLEW UP",
		a.maxSpeed, a.maxWet, a.roughness, a.seconds);
	// Gates at 1.25 x the observed values (the part above the water moves as cloth in vacuum: no air).
	Check(a.ok && a.maxWet < 0.1 && a.roughness < 0.27, "the implicit coupling is stable for a cotton flap", a.error);
	c.coupling = "staggered";
	c.T = 0.3;
	const FlapResult b = RunFlap(c);
	std::printf("  staggered, same case: %s at macro step %d, max |v| %.3g m/s\n", b.ok ? "stable" : "blew up", b.blewAt, b.maxSpeed);
	Check(!b.ok, "the staggered exchange diverges on the same case");
}

// C: a free stiff plate released at rest in the flume (water on both sides): its
// initial acceleration gives an added mass per area that does not depend on its own mass.
void Plate()
{
	std::printf("C: plate added mass\n");
	FlapCase c;
	c.free = true; c.tilt = 0; c.bend = 1.0; c.damping = false; c.cells = 24; c.macro = 1.0/240; c.T = 2*c.macro + 1e-9;
	double ma[2] = {0, 0};
	int k = 0;
	for (double sigma : {0.05, 10.0}) {
		c.density = sigma;
		const FlapResult r = RunFlap(c);
		const double a0 = r.meanVy.size() >= 2 ? -r.meanVy[1]/c.macro : NAN;
		ma[k++] = sigma*(9.81/a0 - 1);
		std::printf("  sigma %g: a0 %.5g m/s^2, added mass %.4g kg/m^2\n", sigma, a0, ma[k - 1]);
	}
	Check(std::fabs(ma[0]/ma[1] - 1) < 0.03, "the plate's added mass does not depend on its mass (3%: 1.25 x observed)");
}

// F: the cloth receives the liquid's impulse exactly: over a macro step of a free sheet
// without damping, contact or pins, sum m dv = H sum m g + the impulses of the liquid's
// steps (binary32 in the payload; the elastic forces sum to the rounding of K0 and g0).
void Exchange()
{
	std::printf("F: the impulse exchange\n");
	FlapCase c;
	c.free = true; c.tilt = 17*Pi/180; c.damping = false; c.cells = 24; c.macro = 1.0/30; c.T = 0.2; c.density = 0.2; c.vertices = 8;
	std::string e;
	auto g = FlapGroup(c, e);
	Check(g != nullptr, "exchange group", e);
	if (!g) return;
	auto snap = g->Snapshot("flap");
	const StaticPayloadResolver rs = [&](const std::string& dg, std::size_t bud, std::shared_ptr<const Bytes>& out, std::string& w) {
		return snap->ReadStatic(dg, bud, out, w) == FrameStatus::Ready; };
	const double area = 0.24*0.24, mass = c.density*area;
	auto momentum = [&](std::shared_ptr<const Bytes> b) -> double {
		MeshFrame m;
		std::string why;
		double p = 0;
		if (!b || !DecodeMesh(*b, budget, rs, m, why)) return NAN;
		// Lumped masses of the 8 x 8 sheet (quarter and half areas at corners and edges).
		for (int j = 0; j < c.vertices; ++j) for (int i = 0; i < c.vertices; ++i) {
			const double w = ((i == 0 || i == c.vertices - 1) ? 0.5 : 1.0)*((j == 0 || j == c.vertices - 1) ? 0.5 : 1.0);
			p += w*mass/double((c.vertices - 1)*(c.vertices - 1))*F(m.velocities[3*std::size_t(i + c.vertices*j) + 1]);
		}
		return p;
	};
	// The cloth's state served at a step's start, and the liquid's steps served to the cloth in that step: the
	// cloth's momentum change from one step's start to the next is H m g plus those steps' impulses.
	double worst = 0, last = NAN, lastI = NAN, residue = 0;
	int checked = 0;
	for (int k = 0; k < 6 && !g->Finished(); ++k) {
		Check(g->StepMacro(e), "exchange step", e);
		const double p = momentum(g->ServedValue(0));
		if (std::isfinite(last) && std::isfinite(lastI)) {
			const double expected = -9.81*mass*c.macro + lastI;
			worst = std::max(worst, std::fabs((p - last) - expected)/(9.81*mass*c.macro));
			if (std::getenv("M7J_F")) std::printf("    macro %d: momentum change %.6e, expected %.6e (impulses %.6e), residual %+.3e N s (%+.2e of H m g)\n", k, p - last, expected, lastI,
				(p - last) - expected, ((p - last) - expected)/(9.81*mass*c.macro));
			++checked;
		}
		last = p;
		FieldFrame f;
		std::string why;
		lastI = 0;
		// m7i: the elements' gradients served with the state (binary32 per element) summed over the sheet: a free
		// sheet's elastic forces sum to zero, so this is their rounding residue (the force the copy feels in total).
		if (const auto el = g->ServedValue(4)) {
			FieldFrame ef;
			if (DecodeField(*el, budget, ef, why))
				for (const auto& ch : ef.channels) if (ch.name == "gradient") {
					double s[3] = {0, 0, 0};
					for (std::size_t q = 0; q + 2 < ch.values.size(); q += 3) for (int a = 0; a < 3; ++a) s[a] += F(ch.values[q + std::size_t(a)]);
					residue = std::max(residue, std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2])/(9.81*mass));
				}
		}
		const auto steps = g->ServedValue(5);   // m7h: the connection after surface_elements
		if (steps && DecodeField(*steps, budget, f, why))
			for (const auto& ch : f.channels) if (ch.name == "impulse") for (std::size_t q = 1; q < ch.values.size(); q += 3) lastI += F(ch.values[q]);
	}
	std::printf("  %d macro steps: |cloth momentum change - (H m g + the liquid's impulses)| <= %.2e of H m g; the served elements' gradients sum to %.2e of m g\n",
		checked, worst, residue);
	// m7j: 1e-6 (m7e-m7i 1e-4): with the elements' blocks balanced (CorotatedElements::Balance) what is left is
	// the binary32 rounding of the exchanged state and impulses (1.6e-9 measured).
	Check(checked >= 3 && worst < 1e-6, "the cloth's momentum changes by exactly the liquid's impulse and its weight");
}



std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const SimulationGroup& G, const char* member, std::int64_t from = 0)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> x;
	for (const auto& e : G.Snapshot(member)->Entries()) if (e.index >= from) x[{e.index, e.output}] = e.digest;
	return x;
}

// G: twins, thread counts, a group checkpoint.
void Determinism()
{
	std::printf("G: determinism\n");
	FlapCase c;
	c.cells = 24; c.T = 0.3; c.density = 0.2;
	std::string e;
	auto run = [&](const FlapCase& k) {
		auto g = FlapGroup(k, e);
		Check(g != nullptr, "determinism group", e);
		while (g && !g->Finished()) if (!g->StepMacro(e)) { Check(false, "determinism step", e); break; }
		return g;
	};
	auto a = run(c), b = run(c);
	FlapCase one = c, three = c;
	one.liquidThreads = 1; one.clothThreads = 1;
	three.liquidThreads = 3; three.clothThreads = 2;
	auto x = run(one), y = run(three);
	if (!a || !b || !x || !y) return;
	for (const char* m : {"pool", "flap"}) {
		Check(Digests(*a, m) == Digests(*b, m), std::string("twin runs publish the same bytes: ") + m);
		Check(Digests(*a, m) == Digests(*x, m) && Digests(*a, m) == Digests(*y, m), std::string("1, 3 and 8 liquid threads (1, 2 and 4 cloth) publish the same bytes: ") + m);
	}
	// A group checkpoint after three macro steps resumes byte for byte.
	{
		auto g = FlapGroup(c, e);
		for (int k = 0; k < 3 && g; ++k) Check(g->StepMacro(e), "first macro steps", e);
		MemoryCheckpointSink sink;
		Check(g && g->CheckpointTo(sink, e), "group checkpoint", e);
		// A fresh group, configured but not started, restored from it.
		const auto frames = std::uint64_t(std::ceil(c.T*30 - 1e-9)) + 1;
		(void)frames;
		auto h = FlapGroupUnstarted(c, e);
		MemoryCheckpointSource source(sink.bytes);
		Check(h && h->RestoreFrom(source, e), "group restore", e);
		while (h && !h->Finished()) if (!h->StepMacro(e)) { Check(false, "resumed step", e); break; }
		for (const char* m : {"pool", "flap"})
			Check(h && Digests(*a, m, 4) == Digests(*h, m, 4) && !Digests(*h, m, 4).empty(), std::string("the resumed group publishes the frames byte for byte: ") + m);
	}
	std::printf("  twins, threads and the group checkpoint compared\n");
}

// H: the guards: a macro step a few ulp short of dividing the frame interval runs as fast as
// the exact one (no sliver segments), and a staggered sheet whose cloth steps first reads the
// cloth's state at the macro step's start (the bytes of the Jacobi order).
void Guards()
{
	std::printf("H: guards\n");
	FlapCase c;
	c.cells = 24; c.T = 0.2; c.coupling = "staggered"; c.density = 500; c.macro = 0.008333333333333333;
	const FlapResult exact = RunFlap(c);
	c.macro = 0.00833333333333333;
	const FlapResult shorter = RunFlap(c);
	std::printf("  macro 0.008333333333333333: %.2f s; 0.00833333333333333: %.2f s\n", exact.seconds, shorter.seconds);
	Check(exact.ok && shorter.ok && shorter.seconds < 1.5*exact.seconds + 0.5, "a macro step a few ulp short costs what the exact one does");
	FlapCase gs = c, jac = c;
	gs.macro = jac.macro = 1.0/60;
	gs.order = {"flap", "pool"};
	jac.jacobi = true;
	std::string e;
	auto run = [&](const FlapCase& k) {
		auto g = FlapGroup(k, e);
		Check(g != nullptr, "order group", e);
		while (g && !g->Finished()) if (!g->StepMacro(e)) { Check(false, "order step", e); break; }
		return g;
	};
	auto a = run(gs), b = run(jac);
	if (a && b)
		for (const char* m : {"pool", "flap"})
			Check(Digests(*a, m) == Digests(*b, m), std::string("cloth first in Gauss-Seidel reads the step-start state (the Jacobi bytes): ") + m);
}

// I: refusals.
void Refusals()
{
	std::printf("I: refusals\n");
	auto liquid = [](V::Members sheet, double viscosity) {
		return V::MapValue({{"frames", V::Unsigned(3)}, {"domain_size", Vec(1, 0.6, 0.4)}, {"resolution", V::Unsigned(8)},
			{"viscosity", V::Float(viscosity)}, {"sheets", V::ArrayValue({V::MapValue(sheet)})}});
	};
	std::string e;
	SolverDescription d;
	auto s = Make(LiquidSolverId);
	V::Members imp{{"name", V::String("f")}, {"motion", V::String("port")}, {"coupling", V::String("implicit")}};
	V::Members relaxed = imp;
	relaxed.emplace_back("relaxation", V::Float(0.5));
	Check(!s->Configure(liquid(relaxed, 0), EmptySceneInputs(), d, e), "an implicit sheet refuses relaxation", e);
	Check(!s->Configure(liquid(imp, 1e-3), EmptySceneInputs(), d, e) && e.find("viscosity") != std::string::npos, "sheets with viscosity are refused", e);
	Check(s->Configure(liquid(imp, 0), EmptySceneInputs(), d, e) && s->Start(e), "an implicit sheet configures", e);
	Check(d.evidence.fidelity == "preview" && d.capabilities.ownedImplicitPorts.size() == 6, "implicit sheets: preview, six owned ports (m7h: the elements)");
	class Null final : public IFrameSink {
	public:
		bool Publish(std::int64_t, const std::string&, double, std::shared_ptr<const Bytes>, const Bytes&, std::string&) override { return true; }
		bool PublishStatic(const std::string&, std::shared_ptr<const Bytes>, std::string& g, std::string&) override { g = "x"; return true; }
	} sink;
	AdvanceResult r;
	Check(!s->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, e), "an implicit sheet refuses to run outside a group", e);
	// m7f: the cloth refuses an implicit sheet's load without the sheet's steps connected (it
	// would apply their mean over its own substeps: the unstable exchange of M7e section 2.1).
	FlapCase f;
	f.cells = 16; f.T = 0.1; f.steps = false;
	const FlapResult fr = RunFlap(f);
	std::printf("  surface_load_steps not connected: %s\n", fr.error.empty() ? "(ran)" : fr.error.c_str());
	Check(!fr.ok && fr.error.find("surface_load_steps is not connected") != std::string::npos, "an implicit sheet without its steps connected is refused", fr.error);
	// m7h: and the liquid refuses an implicit sheet whose elements (the cloth's corotational model) are not connected.
	FlapCase fe;
	fe.cells = 16; fe.T = 0.1; fe.elements = false;
	const FlapResult er = RunFlap(fe);
	std::printf("  surface_elements not connected: %s\n", er.error.empty() ? "(ran)" : er.error.c_str());
	Check(!er.ok && er.error.find("sheet_elements") != std::string::npos && er.error.find("surface_elements") != std::string::npos,
		"an implicit sheet without its elements connected is refused", er.error);
}

// A: the coupled pressure operator with a sheet is symmetric positive definite (dense, 2-D).
void Operator()
{
	std::printf("A: the coupled operator\n");
	MembraneCase c;
	c.cells = 12; c.vertices = 7; c.L = 0.3; c.height = 0.15; c.ym = 0.05; c.d = 0.05;
	MembraneRig r;
	Check(r.Build(c, true, 0), "operator rig", r.error);
	if (!r.ok) return;
	using namespace Liquid;
	auto& sh = r.m.sheets[0];
	std::string e;
	Check(sh.dyn.Prepare(1.0/240, sh.v, {}, sh.triangles, e), "prepare", e);
	SheetCoupling coupling(r.m.GridSpec(), r.m.SheetFaces(), r.m.sheets, 1000);
	const auto& g = r.m.GridSpec();
	std::vector<std::uint8_t> active;
	std::vector<double> x(g.Cells(), 0.0), y;
	PressureOptions po;
	std::vector<IPressureCoupling*> cs{&coupling};
	ApplyPressureOperator(g, r.m.phi, r.m.fraction, po, cs, x, y, active, r.m.ThreadPool());
	std::vector<std::size_t> cells;
	for (std::size_t k = 0; k < g.Cells(); ++k) if (active[k]) cells.push_back(k);
	const std::size_t n = cells.size();
	std::vector<double> A(n*n, 0.0);
	for (std::size_t j = 0; j < n; ++j) {
		std::fill(x.begin(), x.end(), 0.0);
		x[cells[j]] = 1;
		ApplyPressureOperator(g, r.m.phi, r.m.fraction, po, cs, x, y, active, r.m.ThreadPool());
		for (std::size_t i = 0; i < n; ++i) A[i*n + j] = y[cells[i]];
	}
	double asym = 0, amax = 0;
	for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) {
		asym = std::max(asym, std::fabs(A[i*n + j] - A[j*n + i]));
		amax = std::max(amax, std::fabs(A[i*n + j]));
	}
	// Cholesky.
	std::vector<double> L = A;
	bool pd = true;
	double minPivot = 1e300;
	for (std::size_t j = 0; j < n && pd; ++j) {
		double s = L[j*n + j];
		for (std::size_t k = 0; k < j; ++k) s -= L[j*n + k]*L[j*n + k];
		if (!(s > 0)) { pd = false; break; }
		minPivot = std::min(minPivot, s);
		const double d = std::sqrt(s);
		L[j*n + j] = d;
		for (std::size_t i = j + 1; i < n; ++i) {
			double t = L[i*n + j];
			for (std::size_t k = 0; k < j; ++k) t -= L[i*n + k]*L[j*n + k];
			L[i*n + j] = t/d;
		}
	}
	std::printf("  %zu unknowns, %zu sheet faces: max |A - A^T| / max |A| = %.1e, Cholesky %s (min pivot %.3e)\n", n, r.m.SheetFaces().size(),
		asym/amax, pd ? "succeeds" : "FAILS", minPivot);
	Check(asym/amax < 1e-13 && pd, "the coupled operator is symmetric positive definite");
	// m7i: a free sheet (every edge free) in a 3-D tank: faces its edges cover in part.
	FreeSheet f;
	MakeFreeSheet(f, 10, 0.95, 0.46, 6, {{0.5, 0.48, 0.5}}, 20*Pi/180, 0.2);
	Check(f.ok, "free sheet rig", f.error);
	if (!f.ok) return;
	auto& fs = f.model.sheets[0];
	Check(fs.dyn.Prepare(1.0/240, fs.v, {}, fs.triangles, e), "prepare", e);
	std::size_t part = 0, beside = 0;
	for (const auto& c : f.model.SheetFaces()) { part += c.cover < 1; beside += !c.crossed; }
	SheetCoupling fc(f.model.GridSpec(), f.model.SheetFaces(), f.model.sheets, 1000);
	const auto& fg = f.model.GridSpec();
	std::vector<IPressureCoupling*> fcs{&fc};
	std::vector<double> fx(fg.Cells(), 0.0), fy;
	ApplyPressureOperator(fg, f.model.phi, f.model.fraction, po, fcs, fx, fy, active, f.model.ThreadPool());
	std::vector<std::size_t> fcells;
	for (std::size_t k = 0; k < fg.Cells(); ++k) if (active[k]) fcells.push_back(k);
	const std::size_t fn = fcells.size();
	std::vector<double> B(fn*fn, 0.0);
	for (std::size_t j = 0; j < fn; ++j) {
		std::fill(fx.begin(), fx.end(), 0.0);
		fx[fcells[j]] = 1;
		ApplyPressureOperator(fg, f.model.phi, f.model.fraction, po, fcs, fx, fy, active, f.model.ThreadPool());
		for (std::size_t i = 0; i < fn; ++i) B[i*fn + j] = fy[fcells[i]];
	}
	double basym = 0, bmax = 0;
	for (std::size_t i = 0; i < fn; ++i) for (std::size_t j = 0; j < fn; ++j) {
		basym = std::max(basym, std::fabs(B[i*fn + j] - B[j*fn + i]));
		bmax = std::max(bmax, std::fabs(B[i*fn + j]));
	}
	bool bpd = true;
	for (std::size_t j = 0; j < fn && bpd; ++j) {
		double sum = B[j*fn + j];
		for (std::size_t k = 0; k < j; ++k) sum -= B[j*fn + k]*B[j*fn + k];
		if (!(sum > 0)) { bpd = false; break; }
		const double dd = std::sqrt(sum);
		B[j*fn + j] = dd;
		for (std::size_t i = j + 1; i < fn; ++i) {
			double t = B[i*fn + j];
			for (std::size_t k = 0; k < j; ++k) t -= B[i*fn + k]*B[j*fn + k];
			B[i*fn + j] = t/dd;
		}
	}
	std::printf("  free sheet (m7i): %zu unknowns, %zu sheet faces (%zu partly covered, %zu beside its edges): max |A - A^T| / max |A| = %.1e, Cholesky %s\n",
		fn, f.model.SheetFaces().size(), part, beside, basym/bmax, bpd ? "succeeds" : "FAILS");
	Check(part > 0 && beside > 0 && basym/bmax < 1e-13 && bpd, "with free edges the coupled operator is symmetric positive definite");
}

// D: a free stiff plate spanning a 2-D tank under a uniform load p0 with a layer of
// depth d above it, no gravity: a = p0 / (sigma + rho d) exactly (a 1-D piston), and
// the momentum of liquid, the liquid on the closed faces and plate grows by p0 L t.
void Piston()
{
	std::printf("D: piston\n");
	for (double sigma : {0.2, 50.0}) {
		MembraneCase c;
		c.cells = 32; c.g = 0; c.pressure = 20; c.sigma = sigma; c.stretch = 1.0; c.macro = 1.0/120;
		MembraneRig r;
		Check(r.Build(c, false, 10), "piston rig", r.error);
		if (!r.ok) return;
		const double p0 = r.Momentum();
		std::vector<double> vy;
		double worst = 0;
		for (int k = 0; k < 6; ++k) {
			Check(r.Macro(), "piston step", r.error);
			const auto& l = r.m.sheets[0];
			double v = 0;
			for (std::size_t q = 0; q < l.Vertices(); ++q) v += l.v[3*q + 1]/double(l.Vertices());
			vy.push_back(v);
			const double expected = -c.pressure*c.L*r.hz*r.m.Time();
			worst = std::max(worst, std::fabs((r.Momentum() - p0) - expected)/std::fabs(expected));
		}
		const double a = -vy.back()/r.m.Time(), ref = c.pressure/(sigma + 1000*c.d);
		std::printf("  sigma %g: a %.6g m/s^2, p0 / (sigma + rho d) %.6g (error %.2e); momentum audit %.2e of p0 L t\n", sigma, a, ref, a/ref - 1, worst);
		Check(std::fabs(a/ref - 1) < 1e-2, "the piston's acceleration is p0 / (sigma + rho d)");
		Check(worst < 5e-3, "liquid, face liquid and plate gain the load's impulse");
	}
}

// E: the strip's fundamental with the layer above it (no gravity) against the
// potential-flow reference (MembraneFrequencyReference).
void Membrane()
{
	std::printf("E: membrane frequency\n");
	MembraneCase c;
	c.cells = 32; c.g = 0; c.pressure = 5; c.ym = 0.046875; c.T = 1.5;
	MembraneRig r;
	Check(r.Build(c, true, 0), "membrane rig", r.error);
	if (!r.ok) return;
	std::vector<double> t, y;
	while (r.m.Time() < c.T - 1e-9) { Check(r.Macro(), "membrane step", r.error); if (!r.ok) return; t.push_back(r.m.Time()); y.push_back(r.Mid()); }
	const double w = Frequency(t, y), ref = MembraneFrequencyReference(c);
	std::printf("  omega %.4g rad/s, reference %.4g: error %.3g\n", w, ref, w/ref - 1);
	Check(std::fabs(w/ref - 1) < 0.066, "the membrane's frequency in liquid (6.6%: 1.25 x the error observed at 32 to 128 cells)");
}

// J (m7f): the membrane of E sagging 16.5 mm under a 5 cm layer with gravity (M7e's case (b) at
// large sag, which leaked): the mean mid-span sag over [1.5, 3] s through the group against the
// cloth alone under the analytic hydrostatic load.
void LargeSag()
{
	std::printf("J: membrane at large sag\n");
	MembraneCase c;
	c.cells = 32; c.E = 2e4; c.ym = 0.046875; c.T = 3;
	double ys = 0;
	const auto ref = HydrostaticReference(c, ys);
	const MembraneRun r = RunMembrane(c);
	Check(r.ok, "membrane group", r.error);
	if (!r.ok) return;
	const std::size_t a = std::size_t(c.vertices/2), b = std::size_t((c.vertices - 1)/2);
	const double refSag = c.ym - 0.5*(ref[a] + ref[b]);
	double mean = 0;
	int k = 0;
	for (std::size_t j = 0; j < r.t.size(); ++j) if (r.t[j] >= 0.5*c.T) { mean += r.mid[j]; ++k; }
	const double sag = c.ym - mean/std::max(1, k);
	std::printf("  32 cells: sag %.5f m, reference %.5f m: error %.3g\n", sag, refSag, sag/refSag - 1);
	Check(std::fabs(sag/refSag - 1) < 0.02, "the membrane's static sag under a layer that it holds (2%)");
}

// K (m7f): the pinned bag of --cup (24 cells, 6 cm of water, E 2e4 N/m) for 1 s: no liquid below
// the sheet beyond the cells beside it, no wet cell below it, the two components apart, and the
// liquid's sharp volume within 1% of the start (the correction holds the smoothed one).
void Cup()
{
	std::printf("K: a pinned bag holding liquid\n");
	CupCase c;
	c.cells = 24; c.d = 0.06; c.E = 2e4; c.T = 1;
	bool ok = false;
	std::string e;
	const auto s = RunCup(c, 0, ok, e);
	Check(ok, "cup", e);
	if (!ok) return;
	double beyond = 0, wet = 0;
	bool breached = false;
	for (const CupSample& x : s) { beyond = std::max(beyond, x.beyond); wet = std::max(wet, x.bottomWet); breached = breached || x.breached; }
	const double V0 = s[0].top, total = s.back().top + s.back().bottom;
	std::printf("  sag %.4f m; below the sheet beyond its cells %.2e, wet cells below %.2e of V0; sharp volume %+.2e of V0%s\n", s.back().sag,
		beyond/V0, wet/V0, total/V0 - 1, breached ? "; BREACHED" : "");
	Check(!breached && beyond == 0 && wet == 0 && std::fabs(total/V0 - 1) < 0.01, "a pinned bag holds its liquid");
}

// L (m7f): m7e's linear step within a macro step stretches a rotating sheet by (omega H)^2 / 2;
// m7h: the corotational model does not (below 0.5%, against 5.6% for the linear one).
void Rotation()
{
	std::printf("L: rotation within a macro step\n");
	const double H = 1.0/30, omega = 10;
	const double lin = RotatingSheet(omega, H, 4, 1, true, 0.1), non = RotatingSheet(omega, H, 4, 0, true, 0.1);
	const double cor = RotatingSheet(omega, H, 4, 2, true, 0.1), corN = RotatingSheet(omega, H, 4, 2, false, 0.1);
	std::printf("  omega 10 rad/s, H 1/30 s: linear per macro step %.3e, corotational %.3e (normal axis %.3e), nonlinear %.3e, (omega H)^2 / 2 %.3e\n",
		lin, cor, corN, non, 0.5*omega*H*omega*H);
	Check(std::fabs(lin/(0.5*omega*H*omega*H) - 1) < 0.05 && non < 1e-3, "the linear step's stretch is (omega H)^2 / 2");
	Check(cor < 5e-3 && corN < 5e-3, "the corotational step's stretch stays below 0.5%");
}

// M (m7h): the barrier's watertightness check (flood fill) on a sheet pinned all round: 0 holes;
// with two triangles removed from its middle, the fill reaches the back side through the hole.
void Watertight()
{
	std::printf("M: barrier watertightness (flood fill)\n");
	long long result[2] = {-2, -2};
	for (int hole = 0; hole < 2; ++hole) {
		FreeSheet f;
		MakeFreeSheet(f, 32, 0.9, 0.3, 8, {{0.5, 0.5, 0.5}}, 10*Pi/180, 0.2);
		Check(f.ok, "sheet", f.error);
		if (!f.ok) return;
		Liquid::LiquidSheet& sh = f.model.sheets[0];
		for (int j = 0; j <= 8; ++j) for (int i = 0; i <= 8; ++i)
			if (i == 0 || j == 0 || i == 8 || j == 8) sh.dyn.fixed[std::size_t(i + 9*j)] = 1;
		if (hole) {
			// The two triangles of the quad at (4, 4).
			const std::size_t q = 2*std::size_t(4 + 8*4);
			// They close no face (the topology, and so the pinned rim, unchanged).
			sh.open.assign(sh.triangles.size()/3, 0);
			sh.open[q] = sh.open[q + 1] = 1;
			f.model.UpdateSheetGeometry();
		}
		result[hole] = f.model.SheetBarrierHoles();
	}
	std::printf("  pinned all round: %lld holes; with a quad removed (3.75 cm, about a cell): %lld cells reached behind it\n", result[0], result[1]);
	Check(result[0] == 0 && result[1] > 0, "the flood fill finds no hole in an intact pinned sheet and finds a removed quad");
}

void Probe(int argc, char** argv)
{
	FlapCase c;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "density") c.density = std::atof(v.c_str());
		else if (k == "cells") c.cells = std::atoi(v.c_str());
		else if (k == "macro") c.macro = 1/std::atof(v.c_str());
		else if (k == "coupling") c.coupling = v;
		else if (k == "response") c.response = v;
		else if (k == "relaxation") c.relaxation = std::atof(v.c_str());
		else if (k == "T") c.T = std::atof(v.c_str());
		else if (k == "dam") c.dam = v == "1";
		else if (k == "verbose") verbose = v == "1";
		else if (k == "wide") c.wide = v == "1";
		else if (k == "tilt") c.tilt = std::atof(v.c_str())*Pi/180;
		else if (k == "free") c.free = v == "1";
		else if (k == "vertices") c.vertices = std::atoi(v.c_str());
		else if (k == "substeps") c.substeps = std::atoi(v.c_str());
		else if (k == "integrator") c.integrator = v;
		else if (k == "steps") c.steps = v == "1";
	}
	const FlapResult r = RunFlap(c);
	std::printf("density %g cells %d macro 1/%g %s %s: %s steps %d max |v| %.3g m/s (submerged %.3g) roughness %.3g strain %.3g (%.1f s) %s\n", c.density, c.cells,
		1/c.macro, c.coupling.c_str(), c.response.c_str(), r.ok ? "stable" : "BLEW UP", r.blew ? r.blewAt : r.steps, r.maxSpeed, r.maxWet, r.roughness,
		r.maxStrain, r.seconds, r.error.c_str());
}
}

int main(int argc, char** argv)
{
	std::string error;
	Check(RegisterLiquidSolver(error), "register liquid.mac", error);
	Check(RegisterClothSolver(error), "register cloth.shell", error);
	if (argc > 1 && std::string(argv[1]) == "--probe") { Probe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--free") { FreeSheetProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--plate") { PlateProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--copy") { CopyOnlyProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--membrane") { MembraneProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--cup") { CupProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--spin") { SpinProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--air") { AirProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--exchange") { Exchange(); return failures ? 1 : 0; }
	if (argc > 1 && std::string(argv[1]) == "--rotate") { RotateProbe(argc, argv); return 0; }
	if (argc > 1 && std::string(argv[1]) == "--layer") { LayerProbe(argc, argv); return 0; }
	const double t0 = Seconds();
	Operator();
	Stability();
	Plate();
	Piston();
	Membrane();
	Exchange();
	Determinism();
	Guards();
	Refusals();
	LargeSag();
	Cup();
	Rotation();
	Watertight();
	std::printf("LiquidClothCouplingTest: %.1f s\n", Seconds() - t0);
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("LiquidClothCouplingTest: all checks passed\n");
	return 0;
}
