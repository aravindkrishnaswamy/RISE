// liquid.mac sheets: thin open surfaces as moving boundaries (simulation M7d,
// docs/SIMULATION_SOLVER_CONTRACT.md; preview).
//
//   A  crossings: a plane closes exactly the faces between the cell-centre
//      pairs it separates (vertical and tilted, against the plane's sign),
//      a half plane only those whose segment it crosses; any thread count
//   B  leakage: a sheet held across a tank with liquid on one side, 2-D
//      (vertical and tilted) and 3-D (tilted): no cell on the dry side fills;
//      the dry side's smoothed volume stays zero; the wet side's volume
//      change and the hydrostatic load are reported (the load checked within
//      10% of rho g H^2 / 2 with H the liquid's depth at the sheet)
//   C  paddle: a flat sheet oscillating normal to itself in a closed tank full
//      of liquid against a prescribed thin box (M7b, two cells thick) on the
//      same motion: the force's in-phase coefficient has the same sign and is
//      within 20% (the box displaces liquid; the sheet does not)
//   D  the solver: description (preview, condition, scope, ports that match
//      cloth.shell's), refusals, threads (1, 3, 8) and twins byte-equal,
//      stop / restore (in memory and streamed) and rollback equal the
//      uninterrupted run; a port sheet needs a port; no `sheets` key without
//      sheets
//   E  group with cloth.shell (staggered, the load lagged one macro step):
//      completes; twin runs and other thread counts publish the same bytes;
//      both members preview; the liquid keeps its volume; the group
//      checkpoint resumes byte for byte; one-way (no load returned): the
//      cloth's frames are those of the cloth alone
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidParameterSchema.h"
#include "../src/Library/Simulation/SimulationParameterSchema.h"
#include "../src/Library/Simulation/Liquid/LiquidValidation.h"
#include "../src/Library/Simulation/Cloth/ClothSolver.h"
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

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }
V Quat4(double w, double x, double y, double z) { return V::ArrayValue({V::Float(w), V::Float(x), V::Float(y), V::Float(z)}); }

std::unique_ptr<ISimulationSolver> Make(const char* id)
{
	std::string error;
	auto s = CreateSolver(id, error);
	Check(s != nullptr, std::string("create ") + id, error);
	return s;
}

class DigestSink final : public IFrameSink {
public:
	std::map<std::pair<std::int64_t,std::string>,std::string> digests;
	bool Publish(std::int64_t i, const std::string& o, double, std::shared_ptr<const Bytes> p, const Bytes&, std::string&) override {
		digests[{i, o}] = RISE::RISECBOR64::SHA256Hex(*p);
		return true;
	}
	bool PublishStatic(const std::string&, std::shared_ptr<const Bytes> p, std::string& digest, std::string&) override {
		digest = RISE::RISECBOR64::SHA256Hex(*p);
		return true;
	}
};

bool RunAll(ISimulationSolver& s, IFrameSink& sink, std::string& error)
{
	AdvanceResult r;
	for (int k = 0; k < 1000; ++k) {
		if (!s.Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) return false;
		if (r.status == AdvanceResult::Status::Finished) return true;
	}
	error = "did not finish";
	return false;
}

// A rectangle sx x sy in its frame's xy plane (normal +z), nx x ny cells, prescribed at pos with rotation q.
Liquid::LiquidSheet Rect(double sx, double sy, int nx, int ny, const Liquid::V3& pos, const Liquid::Quat& q)
{
	using namespace Liquid;
	LiquidSheet sh;
	sh.name = "sheet";
	for (int j = 0; j <= ny; ++j) for (int i = 0; i <= nx; ++i) {
		sh.local.push_back(-0.5*sx + sx*i/nx);
		sh.local.push_back(-0.5*sy + sy*j/ny);
		sh.local.push_back(0);
	}
	for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
		const std::uint32_t a = std::uint32_t(i + (nx + 1)*j), b = a + 1, c = a + std::uint32_t(nx + 1) + 1, d = a + std::uint32_t(nx + 1);
		for (std::uint32_t k : {a, b, c, a, c, d}) sh.triangles.push_back(k);
	}
	sh.path.motion = BodyMotion::Prescribed;
	sh.path.x0 = pos;
	sh.path.q0 = q;
	sh.PrescribedAt(0);
	return sh;
}
// Local z -> world x (a rotation by 90 degrees about y), then `tilt` about z.
Liquid::Quat FacingX(double tilt)
{
	const Liquid::Quat q{{std::cos(Pi/4), 0, std::sin(Pi/4), 0}};
	return Rigid::QMul(Liquid::Quat{{std::cos(0.5*tilt), 0, 0, std::sin(0.5*tilt)}}, q);
}

// ---------------------------------------------------------------- A

void Crossings()
{
	using namespace Liquid;
	std::printf("A: crossings\n");
	const int n = 32;
	Grid g;
	g.n = {{n, n, n}};
	g.h = 1.0/n;
	Pool one(1), eight(8);
	for (double tilt : {0.0, 0.3}) {
		std::vector<LiquidSheet> sheets{Rect(2, 2, 20, 20, {{0.5 + 0.3/n, 0.5, 0.5}}, FacingX(tilt))};
		std::vector<SheetCrossing> a, b;
		FindSheetCrossings(g, sheets, a, &one);
		FindSheetCrossings(g, sheets, b, &eight);
		bool same = a.size() == b.size();
		for (std::size_t k = 0; same && k < a.size(); ++k)
			same = a[k].axis == b[k].axis && a[k].face == b[k].face && a[k].triangle == b[k].triangle && a[k].velocity == b[k].velocity;
		Check(same, "crossings do not depend on the thread count");
		// Against the plane's sign: a face is closed exactly when its two cell centres lie on opposite sides.
		const V3 nrm{{std::cos(tilt), std::sin(tilt), 0}}, c0{{0.5 + 0.3/n, 0.5, 0.5}};
		std::size_t expected = 0, wrong = 0;
		for (int ax = 0; ax < 3; ++ax) {
			const auto d = g.FaceDims(ax);
			for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
				int idx[3] = {i, j, k};
				if (idx[ax] == 0 || idx[ax] == g.n[ax]) continue;
				const V3 r = g.CellCentre(idx[0], idx[1], idx[2]);
				--idx[ax];
				const V3 l = g.CellCentre(idx[0], idx[1], idx[2]);
				const double sl = (l[0] - c0[0])*nrm[0] + (l[1] - c0[1])*nrm[1], sr = (r[0] - c0[0])*nrm[0] + (r[1] - c0[1])*nrm[1];
				const bool crossed = (sl < 0) != (sr < 0);
				const std::size_t f = g.F(ax, i, j, k);
				const bool closed = std::binary_search(a.begin(), a.end(), SheetCrossing{ax, f}, [](const SheetCrossing& p, const SheetCrossing& q) {
					return p.axis != q.axis ? p.axis < q.axis : p.face < q.face; });
				expected += crossed;
				wrong += crossed != closed;
			}
		}
		std::printf("  tilt %.1f: %zu faces closed, %zu expected, %zu differ\n", tilt, a.size(), expected, wrong);
		Check(wrong == 0 && a.size() == expected, "a plane closes exactly the faces between the cell centres it separates");
	}
	// A half plane (edge at y = 0.5 + 0.2 h): only faces whose segment it crosses.
	{
		LiquidSheet s = Rect(2, 1, 20, 10, {{0.5 + 0.3/n, 0.5 + 0.2/n - 0.5, 0.5}}, FacingX(0));
		std::vector<SheetCrossing> a;
		FindSheetCrossings(g, {s}, a, &eight);
		bool below = true;
		for (const SheetCrossing& c : a) {
			const auto d = g.FaceDims(c.axis);
			const int j = int((c.face/std::size_t(d[0])) % std::size_t(d[1]));
			if (g.CellCentre(0, j, 0)[1] > 0.5 + 0.2/n) below = false;
		}
		Check(below && a.size() == std::size_t(16*n), "a half plane closes only the faces below its edge", std::to_string(a.size()));
	}
}

// ---------------------------------------------------------------- B

struct Sides { double wet = 0, dry = 0, phantom = 0, dryCells = 0; };
// Per side of the plane through c0 with normal nrm: the liquid volume (Heaviside
// clamp(1/2 - phi/h), the cell fraction of a distance function), the dry side's
// share of the solver's smoothed measure (LiquidVolume, eps 1.5 h: the false
// interface at the sheet puts its tail on the dry side) and the dry cells with phi < 0.
Sides Measure(const Liquid::LiquidModel& m, const Liquid::V3& c0, const Liquid::V3& nrm)
{
	const auto& g = m.GridSpec();
	Sides s;
	const double eps = 1.5*g.h, cell = g.h*g.h*(g.n[2] == 1 ? 1.0 : g.h);
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const double p = m.phi[g.C(i, j, k)];
		const double H = std::min(1.0, std::max(0.0, 0.5 - p/g.h));
		const double smooth = p < -eps ? 1.0 : p > eps ? 0.0 : 0.5 - p/(2*eps) - std::sin(Pi*p/eps)/(2*Pi);
		const auto x = g.CellCentre(i, j, k);
		const double side = (x[0] - c0[0])*nrm[0] + (x[1] - c0[1])*nrm[1] + (x[2] - c0[2])*nrm[2];
		if (side < 0) s.wet += H*cell;
		else { s.dry += H*cell; s.phantom += smooth*cell; if (p < 0) s.dryCells += 1; }
	}
	return s;
}

// A tank [0, 1]^d with a plane sheet through c0 (tilted by `tilt` about z from
// facing x) and liquid at rest on its -x side up to y = 0.5, or (`dynamic`) a
// column 0.6 high released two cells from the sheet, which runs up and sloshes
// against it.
void Leak(bool threeD, double tilt, double T, bool dynamic = false)
{
	using namespace Liquid;
	const int n = threeD ? 24 : 32;
	LiquidSetup s;
	s.grid.n = {{n, n, threeD ? n : 1}};
	s.grid.h = 1.0/n;
	s.grid.origin = {{0, 0, threeD ? 0.0 : -0.5/n}};
	const V3 c0{{0.5 + 0.3/n, 0.5, threeD ? 0.5 : 0.0}}, nrm{{std::cos(tilt), std::sin(tilt), 0}};
	const double level = 0.5;
	s.sheets.push_back(Rect(1.4, 1.4, 14, 14, c0, FacingX(tilt)));
	SolverOptions o;
	LiquidModel m;
	std::string error;
	const double t0 = Seconds();
	if (dynamic) {
		Shape col;
		col.type = Shape::Type::Box;
		col.lo = {{-1, -1, -1}};
		col.hi = {{c0[0] - 0.1*std::tan(tilt) - 2.0/n, 0.6, 2}};
		s.liquid.push_back(col);
	}
	Check(m.Initialize(s, o, 8, error), "leak tank", error);
	if (!dynamic) {
		const auto& g = m.GridSpec();
		std::vector<double> phi(g.Cells());
		for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
			const auto x = g.CellCentre(i, j, k);
			const double side = (x[0] - c0[0])*nrm[0] + (x[1] - c0[1])*nrm[1];
			phi[g.C(i, j, k)] = std::max(x[1] - level, side);
		}
		Check(m.SetLevelSet(std::move(phi), error), "leak level set", error);
	}
	const Sides start = Measure(m, c0, nrm);
	Sides worst;
	double force = 0, span = 0;
	std::size_t removed = 0;
	bool ok = m.Run(T, 1.0/30, [&](double t, const StepStats& st) {
		const Sides x = Measure(m, c0, nrm);
		worst.dry = std::max(worst.dry, x.dry);
		worst.phantom = std::max(worst.phantom, x.phantom);
		if (x.dryCells > worst.dryCells && std::getenv("SHEET_DEBUG")) {
			const auto& g = m.GridSpec();
			for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
				const auto c = g.CellCentre(i, j, k);
				const double side = (c[0] - c0[0])*nrm[0] + (c[1] - c0[1])*nrm[1] + (c[2] - c0[2])*nrm[2];
				if (side >= 0 && m.phi[g.C(i, j, k)] < 0) std::printf("    t %.3f dry wet cell %d %d %d phi %.3f h side %.3f h\n", t, i, j, k, m.phi[g.C(i, j, k)]/g.h, side/g.h);
			}
		}
		worst.dryCells = std::max(worst.dryCells, x.dryCells);
		removed += st.sheetParticlesRemoved;
		if (t > 0.5*T) {
			force += m.sheetStepImpulse[0][0]*nrm[0] + m.sheetStepImpulse[0][1]*nrm[1];
			span += st.dt;
		}
	}, error);
	Check(ok, "leak run", error);
	force /= span;
	const Sides end = Measure(m, c0, nrm);
	const double width = threeD ? 1.0 : m.GridSpec().h;
	const double hydro = 1000*9.81*level*level/2*width/std::cos(tilt);
	std::printf("  %s%s tilt %.1f, %.1f s: dry side %.2e of the volume at the start, at most %.2e (%g cells wet; the solver's smoothed measure %.2e), wet side "
		"%+.2e; mean load over the second half %.4g N against rho g H^2/(2 cos) = %.4g N (%+.1f%%); %zu particles removed; %.1f s\n",
		threeD ? "3-D" : "2-D", dynamic ? " sloshing" : "", tilt, T, start.dry/start.wet, worst.dry/start.wet, worst.dryCells, worst.phantom/start.wet, end.wet/start.wet - 1, force, hydro,
		100*(force/hydro - 1), removed, Seconds() - t0);
	Check(worst.dryCells == 0, "no cell on the dry side fills");
	// The dry side's share at the start is the half cell the interface at the sheet straddles (phi = the distance to the plane).
	if (dynamic) {
		// No liquid at the start on the dry side: what the measure finds there is the half cell beside the sheet
		// that the false interface straddles once the liquid has reached it.
		Check(worst.dry < 2e-2*start.wet, "the dry side holds no more than the false interface's half cell", std::to_string(worst.dry/start.wet));
		return;
	}
	Check(worst.dry <= start.dry*(1 + 1e-9), "the dry side's volume does not grow", std::to_string((worst.dry - start.dry)/start.wet));
	Check(std::fabs(end.wet/start.wet - 1) < 1e-2, "the wet side keeps its volume within 1%", std::to_string(end.wet/start.wet - 1));
	Check(std::fabs(force/hydro - 1) < 0.1, "the sheet's load is the hydrostatic thrust within 10%", std::to_string(force/hydro - 1));
}

void Leaks()
{
	std::printf("B: leakage\n");
	Leak(false, 0.0, 2.0);
	Leak(false, 0.3, 2.0);
	Leak(true, 0.3, 1.0);
	Leak(false, 0.3, 2.0, true);
	Leak(true, 0.3, 1.0, true);
}

// ---------------------------------------------------------------- C

double Paddle(bool sheet, double& noise)
{
	using namespace Liquid;
	const int n = 32;
	const double A = 0.02, f = 1.0, L = 0.3, w = 2*Pi*f;
	LiquidSetup s;
	s.grid.n = {{n, n, n}};
	s.grid.h = 1.0/n;
	s.gravity = {{0, 0, 0}};
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, 2, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	const V3 c0{{0.5 + 0.21/n, 0.5 + 0.13/n, 0.5 + 0.17/n}};
	if (sheet) {
		LiquidSheet sh = Rect(L, L, 24, 24, c0, FacingX(0));
		sh.path.amplitude = {{A, 0, 0}};
		sh.path.frequency = f;
		s.sheets.push_back(sh);
	} else {
		BodyShape box;
		box.type = BodyShape::Type::Box;
		box.size = {{2.0/n, L, L}};
		LiquidBody b = MakeBody(box, BodyMotion::Prescribed, 1, c0, s.gravity);
		b.amplitude = {{A, 0, 0}};
		b.frequency = f;
		s.bodies.push_back(b);
	}
	SolverOptions o;
	o.particles = false;
	LiquidModel m;
	std::string error;
	Check(m.Initialize(s, o, 8, error), "paddle", error);
	double sinc = 0, tt = 0, peak = 0;
	Check(m.Run(2/f, 1.0/30, [&](double t, const StepStats& st) {
		const double F = sheet ? m.sheetStepImpulse[0][0]/st.dt : m.bodyImpulse[0][0]/st.dt;
		sinc += F*std::sin(w*(t - 0.5*st.dt))*st.dt;
		tt += st.dt;
		peak = std::max(peak, std::fabs(F));
	}, error), "paddle run", error);
	sinc *= 2/tt;
	noise = peak/std::fabs(sinc);
	return sinc/(A*w*w);   // the added mass (the force is m_a A w^2 sin wt against the motion)
}

void Paddles()
{
	std::printf("C: paddle against the thin box\n");
	const double t0 = Seconds();
	double ns = 0, nb = 0;
	const double ms = Paddle(true, ns), mb = Paddle(false, nb);
	const double disk = 8.0/3*1000*std::pow(0.3/std::sqrt(Pi), 3);
	std::printf("  added mass: sheet %.4g kg, thin box %.4g kg (ratio %.3f; a disk of equal area unbounded %.4g); peak / amplitude "
		"sheet %.2f, box %.2f; %.1f s\n", ms, mb, ms/mb, disk, ns, nb, Seconds() - t0);
	Check(ms > 0 && mb > 0, "the force opposes the acceleration for both (positive added mass)");
	Check(std::fabs(ms/mb - 1) < 0.2, "the sheet's added mass is the thin box's within 20%", std::to_string(ms/mb));
}

// ---------------------------------------------------------------- D

V::Members TankParams(int n, std::uint64_t frames)
{
	return {{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(1, 0.6, 0.4)},
		{"resolution", V::Unsigned(std::uint64_t(n))},
		{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(-1, -1, -1)}, {"max", Vec(0.3, 0.45, 2)}})})}};
}
V PaddleSheet()
{
	return V::MapValue({{"name", V::String("paddle")}, {"size", V::ArrayValue({V::Float(0.3), V::Float(0.3)})},
		{"divisions", V::ArrayValue({V::Unsigned(6), V::Unsigned(6)})}, {"position", Vec(0.55, 0.2, 0.2)},
		{"orientation", Quat4(std::cos(Pi/4), 0, std::sin(Pi/4), 0)}, {"amplitude", Vec(0.03, 0, 0)}, {"frequency", V::Float(2)}});
}
V With(V::Members m, const char* key, V value)
{
	for (auto& e : m) if (e.first == key) { e.second = value; return V::MapValue(m); }
	m.emplace_back(key, value);
	return V::MapValue(m);
}

void Solver()
{
	std::printf("D: the solver\n");
	std::string error;
	V::Members p = TankParams(24, 5);
	p.emplace_back("sheets", V::ArrayValue({PaddleSheet()}));
	auto s = Make(LiquidSolverId);
	SolverDescription d;
	Check(s->Configure(V::MapValue(p), EmptySceneInputs(), d, error), "configure a prescribed sheet", error);
	Check(d.evidence.fidelity == "preview" && d.capabilities.fidelityLimit.find("sheets") != std::string::npos &&
		d.evidence.scope.find("M7d") != std::string::npos, "sheets label preview with the reason in the scope");
	bool condition = false;
	for (const auto& c : d.evidence.conditions) if (c.first == "sheets") condition = c.second == std::vector<std::string>{"prescribed"};
	Check(condition, "the sheets condition");
	Check(d.version == "m7m.1", "version m7m.1", d.version);
	{
		SolverDescription plain;
		Check(s->Configure(V::MapValue(TankParams(24, 5)), EmptySceneInputs(), plain, error) && !plain.effectiveParameters.Find("sheets") &&
			plain.evidence.fidelity == "predictive", "without sheets: no sheets key and the m7c label", error);
	}
	// A port sheet's ports are cloth.shell's (a direct connection needs no adapter).
	{
		V::Members q = TankParams(24, 5);
		q.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("curtain")}, {"motion", V::String("port")}})}));
		SolverDescription liquid, cloth;
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), liquid, error), "port sheet", error);
		auto c = Make(ClothSolverId);
		Check(c->Configure(V::MapValue({{"frames", V::Unsigned(5)}}), EmptySceneInputs(), cloth, error), "cloth", error);
		const PortDeclaration *accept = nullptr, *offer = nullptr, *state = nullptr, *load = nullptr;
		for (const auto& x : liquid.ports) { if (x.name == "sheet_state") accept = &x; if (x.name == "sheet_load") offer = &x; }
		for (const auto& x : cloth.ports) { if (x.name == "surface_state") state = &x; if (x.name == "surface_load") load = &x; }
		std::string why;
		Check(accept && offer && state && load && PortsCompatible(*state, *accept, why) && PortsCompatible(*offer, *load, why),
			"the sheet's ports match cloth's surface_state and surface_load", why);
		auto alone = Make(LiquidSolverId);
		DigestSink sink;
		Check(alone->Configure(V::MapValue(q), EmptySceneInputs(), liquid, error) && alone->Start(error) && !RunAll(*alone, sink, error) &&
			error.find("port") != std::string::npos, "a port sheet outside a group or feed is refused at its first step", error);
	}
	// Refusals.
	auto refused = [&](V::Members q, const std::string& what, const std::string& needle) {
		SolverDescription x;
		const bool bad = !s->Configure(V::MapValue(q), EmptySceneInputs(), x, error);
		Check(bad && error.find(needle) != std::string::npos, "refused: " + what, error);
	};
	{
		// m7m: a prescribed sheet with viscosity configures (a no-slip wall of the viscous solve); a staggered port
		// sheet with viscosity is refused (its load is the pressure only).
		V::Members q = p;
		q.emplace_back("viscosity", V::Float(1e-3));
		SolverDescription x;
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), x, error), "a prescribed sheet with viscosity configures (m7m)", error);
		V::Members r = TankParams(24, 5);
		r.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("f")}, {"motion", V::String("port")}})}));
		r.emplace_back("viscosity", V::Float(1e-3));
		refused(r, "a staggered port sheet with viscosity", "viscosity");
	}
	{
		V::Members q = TankParams(24, 5);
		q.emplace_back("sheets", V::ArrayValue({PaddleSheet(), PaddleSheet()}));
		refused(q, "duplicate sheet names", "unique");
		V::Members r = TankParams(24, 5);
		r.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("a")}, {"motion", V::String("port")}, {"port", V::String("body_state")}})}));
		refused(r, "a sheet port named like a body port", "unique");
		V::Members t = TankParams(24, 5);
		t.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("a")}, {"size", V::ArrayValue({V::Float(-1), V::Float(1)})}})}));
		refused(t, "a negative size", "size");
		V::Members u = TankParams(24, 5);
		u.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("a")}, {"motion", V::String("port")}, {"relaxation", V::Float(0)}})}));
		refused(u, "relaxation 0", "relaxation");
		V::Members w = TankParams(24, 5);
		w.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("a")}, {"colour", V::String("red")}})}));
		refused(w, "an unknown key", "unknown");
	}
	// The chunk field `sheet`.
	{
		const ParameterSchema& schema = LiquidParameterSchema();
		V params;
		std::vector<std::string> inputs;
		Check(TranslateSchemaChunk(schema, {{"sheet", {"curtain port load_port curtain_load relaxation 0.5",
			"paddle rect 0.3 0.3 divisions 6 6 at 0.55 0.2 0.2 orientation 0.7071 0 0.7071 0 amplitude 0.03 0 0 frequency 2"}}}, params, inputs, error) &&
			params.Find("sheets") && params.Find("sheets")->GetArray().size() == 2, "sheet lines translate", error);
		V::Members q = TankParams(24, 2);
		if (params.Find("sheets")) q.emplace_back("sheets", *params.Find("sheets"));
		SolverDescription x;
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), x, error), "the translated sheets configure", error);
		bool load = false;
		for (const auto& port : x.ports) if (port.name == "curtain_load" && port.offers) load = true;
		Check(load, "the translated load port is offered");
		Check(!TranslateSchemaChunk(schema, {{"sheet", {"a rect 0.3"}}}, params, inputs, error), "a sheet without its size is refused");
		Check(!TranslateSchemaChunk(schema, {{"sheet", {"a port colour red"}}}, params, inputs, error), "an unknown sheet option is refused");
	}
	// Threads and twins.
	DigestSink reference;
	for (int threads : {1, 3, 8, 8}) {
		auto x = Make(LiquidSolverId);
		SolverDescription dx;
		DigestSink sink;
		Check(x->Configure(With(p, "threads", V::Unsigned(std::uint64_t(threads))), EmptySceneInputs(), dx, error) && x->Start(error) &&
			RunAll(*x, sink, error), "run", error);
		if (threads == 1) reference = sink;
		else Check(sink.digests == reference.digests, std::to_string(threads) + " threads (and the twin) publish the bytes of one thread");
	}
	// Stop / restore and rollback.
	for (int streamed = 0; streamed < 2; ++streamed) {
		auto a = Make(LiquidSolverId);
		SolverDescription da;
		DigestSink first;
		Check(a->Configure(V::MapValue(p), EmptySceneInputs(), da, error) && a->Start(error), "configure", error);
		AdvanceResult r;
		Check(a->Advance(AdvanceRequest{}, first, nullptr, nullptr, r, error) && a->Advance(AdvanceRequest{}, first, nullptr, nullptr, r, error), "two frames", error);
		class StopAfter final : public IProgress {
		public:
			int calls = 0;
			bool Report(double, double, const std::string&) override { return ++calls < 4; }
		} stop;
		Check(a->Advance(AdvanceRequest{}, first, nullptr, &stop, r, error) && r.status == AdvanceResult::Status::Stopped, "stopped mid-segment", error);
		auto b = Make(LiquidSolverId);
		SolverDescription db;
		Check(b->Configure(V::MapValue(p), EmptySceneInputs(), db, error), "configure the restore", error);
		if (streamed) {
			MemoryCheckpointSink sink;
			Check(a->CheckpointTo(sink, error), "streamed checkpoint", error);
			MemoryCheckpointSource source(sink.bytes);
			Check(b->RestoreFrom(source, error), "streamed restore", error);
		} else {
			Bytes state;
			Check(a->Checkpoint(state, error) && b->Restore(state, error), "checkpoint and restore", error);
		}
		DigestSink rest;
		Check(RunAll(*b, rest, error), "finish after restore", error);
		auto merged = first.digests;
		for (const auto& e : rest.digests) merged[e.first] = e.second;
		Check(merged == reference.digests, std::string(streamed ? "streamed" : "in-memory") + " stop / restore with a sheet equals the uninterrupted run");
	}
	{
		auto rb = Make(LiquidSolverId);
		SolverDescription dr;
		DigestSink one, two;
		Check(rb->Configure(V::MapValue(p), EmptySceneInputs(), dr, error) && rb->Start(error), "rollback configure", error);
		AdvanceRequest q;
		q.mode = AdvanceRequest::Mode::MacroStep;
		q.macroEnd = dr.cache.interval;
		AdvanceResult r;
		Check(rb->Advance(q, one, nullptr, nullptr, r, error), "first", error);
		std::shared_ptr<const SolverStateToken> token;
		Check(rb->SaveState(token, error), "save", error);
		q.macroEnd = 3*dr.cache.interval;
		Check(rb->Advance(q, one, nullptr, nullptr, r, error) && rb->RestoreState(*token, error) && rb->Advance(q, two, nullptr, nullptr, r, error), "repeat", error);
		Check(one.digests.at({3, "surface"}) == two.digests.at({3, "surface"}) && two.digests.at({3, "surface"}) == reference.digests.at({3, "surface"}),
			"the repeated macro step after a rollback is identical");
	}
}

// ---------------------------------------------------------------- E

struct GroupRun {
	std::unique_ptr<SimulationGroup> group;
	bool ok = false;
	std::string error;
};

V::Members ClothParams(std::uint64_t frames, double density, unsigned threads)
{
	return {{"frames", V::Unsigned(frames)}, {"size_u", V::Float(0.44)}, {"size_v", V::Float(0.45)},
		{"vertices_u", V::Unsigned(9)}, {"vertices_v", V::Unsigned(9)}, {"origin", Vec(0.6123, 0.55, -0.02)},
		{"axis_u", Vec(0, 0, 1)}, {"axis_v", Vec(0, -1, 0)}, {"threads", V::Unsigned(threads)},
		{"material", V::MapValue({{"base", V::String("cotton-plain-weave")}, {"density", V::Float(density)}})},
		{"pins", V::ArrayValue({V::MapValue({{"select", V::String("v0")}})})}};
}

GroupRun Group(double T, unsigned liquidThreads, unsigned clothThreads, bool twoWay, bool start = true)
{
	GroupRun g;
	const auto frames = std::uint64_t(std::llround(T*30)) + 1;
	V::Members liquid = TankParams(24, frames);
	liquid.emplace_back("threads", V::Unsigned(liquidThreads));
	liquid.emplace_back("sheets", V::ArrayValue({V::MapValue({{"name", V::String("curtain")}, {"motion", V::String("port")}})}));
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"pool", "curtain"};
	g.group = std::make_unique<SimulationGroup>();
	auto& G = *g.group;
	// A heavy curtain (500 kg/m^2: inside the stable range of the lagged exchange, docs section 6).
	g.ok = G.AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), g.error) &&
		G.AddMember("curtain", Make(ClothSolverId), V::MapValue(ClothParams(frames, 500, clothThreads)), g.error) &&
		G.Connect({"curtain", "surface_state", "pool", "sheet_state"}, g.error) &&
		(!twoWay || G.Connect({"pool", "sheet_load", "curtain", "surface_load"}, g.error)) &&
		G.Configure(p, EmptySceneInputs(), g.error) && (!start || G.Start(g.error));
	return g;
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const SimulationGroup& G, const char* member, std::int64_t from = 0)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> x;
	for (const auto& e : G.Snapshot(member)->Entries()) if (e.index >= from) x[{e.index, e.output}] = e.digest;
	return x;
}

void Groups()
{
	std::printf("E: group with cloth.shell\n");
	const double t0 = Seconds();
	std::string e;
	GroupRun a = Group(0.6, 8, 4, true), b = Group(0.6, 8, 4, true), c = Group(0.6, 1, 1, true);
	Check(a.ok && b.ok && c.ok, "the groups configure", a.error + b.error + c.error);
	if (!(a.ok && b.ok && c.ok)) return;
	Check(a.group->MemberFidelity("pool") == "preview" && a.group->MemberFidelity("curtain") == "preview", "both members are preview");
	for (GroupRun* g : {&a, &b, &c}) while (g->ok && !g->group->Finished()) { g->ok = g->group->StepMacro(e); Check(g->ok, "the group completes", e); }
	for (const char* m : {"pool", "curtain"}) {
		Check(Digests(*a.group, m) == Digests(*b.group, m), std::string("twin group runs publish the same bytes: ") + m);
		Check(Digests(*a.group, m) == Digests(*c.group, m), std::string("1-thread members publish the bytes of 8 and 4: ") + m);
	}
	// The curtain moved (the load reached it) and stayed finite.
	{
		const auto s = a.group->Snapshot("curtain");
		const std::int64_t last = std::int64_t(std::llround(0.6*30));
		std::shared_ptr<const Bytes> f0, f1;
		std::string why;
		MeshFrame m0, m1;
		const StaticPayloadResolver res = [&](const std::string& dg, std::size_t bud, std::shared_ptr<const Bytes>& out, std::string& w) {
			return s->ReadStatic(dg, bud, out, w) == FrameStatus::Ready; };
		Check(s->Read(0, "surface", budget, f0, why) == FrameStatus::Ready && s->Read(last, "surface", budget, f1, why) == FrameStatus::Ready &&
			DecodeMesh(*f0, budget, res, m0, why) && DecodeMesh(*f1, budget, res, m1, why), "curtain frames", why);
		double moved = 0;
		bool finite = true;
		for (std::size_t k = 0; k < m1.positions.size() && k < m0.positions.size(); ++k) {
			float x0, x1;
			std::memcpy(&x0, &m0.positions[k], 4);
			std::memcpy(&x1, &m1.positions[k], 4);
			finite = finite && std::isfinite(x1);
			moved = std::max(moved, double(std::fabs(x1 - x0)));
		}
		std::printf("  the curtain moved by up to %.3f m in 0.6 s\n", moved);
		Check(finite && moved > 1e-3 && moved < 0.5, "the liquid pushes the curtain (by a finite amount)", std::to_string(moved));
	}
	// The group checkpoint resumes byte for byte.
	{
		GroupRun x = Group(0.6, 8, 4, true, true);
		for (int k = 0; k < 6 && x.ok; ++k) Check(x.group->StepMacro(e), "first macro steps", e);
		MemoryCheckpointSink sink;
		Check(x.group->CheckpointTo(sink, e), "group checkpoint", e);
		GroupRun y = Group(0.6, 8, 4, true, false);
		MemoryCheckpointSource source(sink.bytes);
		Check(y.ok && y.group->RestoreFrom(source, e), "group restore", e);
		while (y.ok && !y.group->Finished()) Check(y.group->StepMacro(e), "resumed macro", e);
		for (const char* m : {"pool", "curtain"}) {
			auto ref = Digests(*a.group, m, 7);
			Check(!ref.empty() && ref == Digests(*y.group, m, 7), std::string("the resumed group publishes the frames byte for byte: ") + m);
		}
	}
	// One-way: the cloth moves the liquid and receives no load; its frames are the cloth's alone.
	{
		GroupRun one = Group(0.3, 8, 4, false);
		Check(one.ok, "one-way group", one.error);
		while (one.ok && !one.group->Finished()) Check(one.group->StepMacro(e), "one-way macro", e);
		auto cloth = Make(ClothSolverId);
		SolverDescription d;
		DigestSink sink;
		Check(cloth->Configure(V::MapValue(ClothParams(10, 500, 4)), EmptySceneInputs(), d, e) && cloth->Start(e) && RunAll(*cloth, sink, e), "cloth alone", e);
		std::map<std::pair<std::int64_t,std::string>,std::string> alone(sink.digests.begin(), sink.digests.end());
		Check(one.ok && Digests(*one.group, "curtain") == alone, "one-way: the cloth's frames are those of the cloth alone");
	}
	std::printf("  groups %.1f s\n", Seconds() - t0);
}
}

int main(int argc, char** argv)
{
	(void)argc; (void)argv;
	std::string error;
	Check(RegisterLiquidSolver(error), "register liquid.mac", error);
	Check(RegisterClothSolver(error), "register cloth.shell", error);
	const double t0 = Seconds();
	Crossings();
	Leaks();
	Paddles();
	Solver();
	Groups();
	std::printf("LiquidSheetTest: %.1f s\n", Seconds() - t0);
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("LiquidSheetTest: all checks passed\n");
	return 0;
}
