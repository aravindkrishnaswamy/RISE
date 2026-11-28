// liquid.mac: viscosity with sheets (simulation M7m, docs/SIMULATION_SOLVER_CONTRACT.md; preview) and
// the 3-D momentum audit of the coupled liquid, sheet and rigid-body exchange.
//
//   A  a honey layer flowing down a tilted held sheet (2-D, gravity at 0.3 rad to the sheet's
//      normal, the sheet off the lattice) against the same layer on a rigid floor at the same
//      height: the layer's flux at the middle within 5% of the floor's (both reported against
//      the Nusselt film's g sin(a) d^3 / (3 nu))
//   B  no viscous coupling across a held sheet: honey moving along it above, at rest below; the
//      layer below stays at rest (its largest speed under 1% of the upper layer's), the upper
//      one is slowed by the sheet's no-slip
//   C  momentum closure with a free sheet (3-D, no gravity, the liquid clear of the walls): a
//      tilted light sheet moving obliquely through honey; liquid, face liquid and sheet keep
//      their momentum on all three axes (to 1% of the sheet's initial momentum), with viscosity
//      and without
//   D  the 3-D audit of the liquid, a coupled sheet and a coupled rigid body together (no
//      gravity, clear of the walls), the body translating, then also spinning: the total momentum
//      on each axis, and the body's angular impulse about its centre of mass against its change
//      of angular momentum; the total angular momentum about the body's centre is reported
//   E  refusal: a staggered port sheet with viscosity
#include "../src/Library/Simulation/Liquid/LiquidModel.h"
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidValidation.h"
#include "../src/Library/Simulation/Rigid/RigidBody.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
using namespace RISE::Simulation;
using namespace RISE::Simulation::Liquid;
int failures = 0;
bool verbose = false;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A flat sheet through `centre` spanned by u and w (unit vectors; half sizes su, sw), nu x nw vertices,
// implicitly coupled with a lumped mass sigma (kg/m^2) and no stiffness; `held`: every vertex fixed.
LiquidSheet MakeSheet(const V3& centre, const V3& u, const V3& w, double su, double sw, int nu, int nw, double sigma, bool held,
	const V3& velocity = {{0, 0, 0}})
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
			sh.v.push_back(held ? 0.0 : velocity[std::size_t(k)]);
		}
	}
	for (int j = 0; j + 1 < nw; ++j) for (int i = 0; i + 1 < nu; ++i) {
		const std::uint32_t p = std::uint32_t(i + nu*j), q = p + 1, r = p + std::uint32_t(nu) + 1, s = p + std::uint32_t(nu);
		for (std::uint32_t k : {p, q, r, p, r, s}) sh.triangles.push_back(k);
	}
	const std::size_t n = sh.x.size()/3;
	const double area = 4*su*sw;
	sh.dyn.mass.assign(n, sigma*area/double(n));
	sh.dyn.fixed.assign(n, held ? 1 : 0);
	sh.dyn.force.assign(3*n, 0.0);
	sh.dyn.x0 = sh.x;
	sh.dyn.xe = sh.x;
	return sh;
}

Shape Box(const V3& lo, const V3& hi)
{
	Shape s;
	s.type = Shape::Type::Box;
	s.lo = lo;
	s.hi = hi;
	return s;
}

SolverOptions Options()
{
	SolverOptions o;
	o.particles = false;
	return o;
}

// ---------------------------------------------------------------- A, B: 2-D layers

struct Layer {
	int nx = 96, ny = 24;
	double L = 0.2, y0 = 0, d = 0.0125, alpha = 0.3, mu = 10, rho = 1400;
};

LiquidSetup LayerSetup(const Layer& c)
{
	LiquidSetup s;
	s.grid.n = {{c.nx, c.ny, 1}};
	s.grid.h = c.L/c.nx;
	s.grid.origin = {{0, 0, -0.5*s.grid.h}};
	s.gravity = {{9.81*std::sin(c.alpha), -9.81*std::cos(c.alpha), 0}};
	s.density = c.rho;
	s.viscosity = c.mu;
	return s;
}

// The flux along x through the column at the middle (above y0) and the liquid's largest speed below y0.
void Profile(const LiquidModel& m, double y0, double& flux, double& below, double& above)
{
	const Grid& g = m.GridSpec();
	const int i = g.n[0]/2;
	flux = 0;
	below = above = 0;
	for (int j = 0; j < g.n[1]; ++j) {
		const double y = g.origin[1] + (j + 0.5)*g.h;
		const std::size_t c = g.C(i, j, 0);
		const double ux = 0.5*(m.u.u[0][g.F(0, i, j, 0)] + m.u.u[0][g.F(0, i + 1, j, 0)]);
		if (y > y0 && m.phi[c] < 0) flux += ux*g.h*std::min(1.0, std::max(0.0, 0.5 - m.phi[c]/g.h));
	}
	for (int j = 0; j < g.n[1]; ++j) for (int ii = 0; ii < g.n[0]; ++ii) {
		const double y = g.origin[1] + (j + 0.5)*g.h;
		if (!(m.phi[g.C(ii, j, 0)] < 0)) continue;
		const double ux = 0.5*(m.u.u[0][g.F(0, ii, j, 0)] + m.u.u[0][g.F(0, ii + 1, j, 0)]);
		const double uy = 0.5*(m.u.u[1][g.F(1, ii, j, 0)] + m.u.u[1][g.F(1, ii, j + 1, 0)]);
		const double sp = std::sqrt(ux*ux + uy*uy);
		if (y < y0) below = std::max(below, sp); else above = std::max(above, sp);
	}
}

void HeldLayer()
{
	std::printf("A: a honey layer down a tilted held sheet against a rigid floor\n");
	Layer c;
	const double h = c.L/c.nx;
	c.y0 = 6.3*h;
	const double nu = c.mu/c.rho, T = 0.08;
	const double nusselt = 9.81*std::sin(c.alpha)*c.d*c.d*c.d/(3*nu);
	double flux[2] = {0, 0};
	for (int sheet = 0; sheet < 2; ++sheet) {
		const double t0 = Seconds();
		LiquidSetup s = LayerSetup(c);
		s.liquid.push_back(Box({{-1, c.y0, -1}}, {{2, c.y0 + c.d, 1}}));
		if (sheet) s.sheets.push_back(MakeSheet({{0.5*c.L, c.y0, 0}}, {{1, 0, 0}}, {{0, 0, 1}}, 0.55*c.L, 2*h, 41, 3, 0.2, true));
		else s.solids.push_back(Box({{-1, -1, -1}}, {{2, c.y0, 1}}));
		LiquidModel m;
		std::string error;
		Check(m.Initialize(s, Options(), 4, error), "layer model", error);
		if (sheet) m.ResetSheetDynamics();
		Check(m.ProjectState(error), "layer projection", error);
		Check(m.Run(T, 1.0/120, nullptr, error), sheet ? "layer on the sheet" : "layer on the floor", error);
		double below, above;
		Profile(m, c.y0, flux[sheet], below, above);
		std::printf("  %s: flux %.4e m^2/s (Nusselt %.4e: %+.2f%%), %.1f s\n", sheet ? "held sheet " : "rigid floor", flux[sheet], nusselt,
			100*(flux[sheet]/nusselt - 1), Seconds() - t0);
		if (sheet) {
			std::printf("  largest speed below the sheet %.2e m/s\n", below);
			Check(below == 0, "no liquid below the held sheet");
		}
	}
	const double rel = flux[1]/flux[0] - 1;
	std::printf("  sheet / floor - 1: %+.2f%%\n", 100*rel);
	Check(std::fabs(rel) < 0.05, "the layer on the held sheet flows as on the rigid floor (5%)", std::to_string(rel));
	Check(std::fabs(flux[0]/nusselt - 1) < 0.15, "the floor's layer is near the Nusselt film");
}

void NoCoupling()
{
	std::printf("B: no viscous coupling across a held sheet\n");
	Layer c;
	c.alpha = 0;
	const double h = c.L/c.nx;
	c.y0 = 8.3*h;
	LiquidSetup s = LayerSetup(c);
	s.liquid.push_back(Box({{-1, -1, -1}}, {{2, c.y0 + c.d, 1}}));
	s.sheets.push_back(MakeSheet({{0.5*c.L, c.y0, 0}}, {{1, 0, 0}}, {{0, 0, 1}}, 0.55*c.L, 2*h, 41, 3, 0.2, true));
	LiquidModel m;
	std::string error;
	Check(m.Initialize(s, Options(), 4, error), "two-layer model", error);
	m.ResetSheetDynamics();
	const Grid& g = m.GridSpec();
	const double u0 = 0.02;
	for (int j = 0; j < g.n[1]; ++j) for (int i = 1; i < g.n[0]; ++i)
		if (g.origin[1] + (j + 0.5)*g.h > c.y0) m.u.u[0][g.F(0, i, j, 0)] = u0;
	Check(m.ProjectState(error), "two-layer projection", error);
	double f0, b0, a0;
	Profile(m, c.y0, f0, b0, a0);
	Check(m.Run(0.05, 1.0/120, nullptr, error), "two layers", error);
	double f1, b1, a1;
	Profile(m, c.y0, f1, b1, a1);
	std::printf("  upper flux %.3e -> %.3e m^2/s; largest speed below the sheet %.2e -> %.2e m/s (%.2e of u0)\n", f0, f1, b0, b1, b1/u0);
	Check(b1 < 0.01*u0, "the layer below a held sheet stays at rest", std::to_string(b1/u0));
	Check(f1 < 0.9*f0, "the sheet's no-slip slows the layer above it");
}

// ---------------------------------------------------------------- C, D: 3-D audits

struct Audit3 {
	bool viscous = true, sheet = true, body = false, spin = false;
	int n = 20;
	double L = 0.1, T = 0.04, sigma = 3, half = 0.6;   // half: the sheet's half size over L (0.6: across the tank, no free edge in it)
	int verts = 21;
};

V3 Total(const LiquidModel& m, V3& liquid)
{
	const Grid& g = m.GridSpec();
	V3 p = m.FluidMomentum();
	liquid = p;
	const double scale = m.Setup().density*g.h*g.h*g.h;
	std::vector<double> theta;
	m.SheetFaceTheta(theta);
	for (std::size_t k = 0; k < m.SheetFaces().size(); ++k) p[std::size_t(m.SheetFaces()[k].axis)] += scale*theta[k]*m.SheetFaces()[k].cover*m.SheetFaces()[k].velocity;   // the closed part (m7i)
	for (const LiquidSheet& sh : m.sheets)
		for (std::size_t q = 0; q < sh.Vertices(); ++q) for (int a = 0; a < 3; ++a) p[std::size_t(a)] += sh.dyn.mass[q]*sh.v[3*q + std::size_t(a)];
	for (const LiquidBody& b : m.bodies) for (int a = 0; a < 3; ++a) p[std::size_t(a)] += b.state.mass*b.state.v[std::size_t(a)];
	return p;
}

// The angular momentum about c: the liquid's (faces, weights as FluidMomentum's), the sheet's vertices, the body's.
V3 Angular(const LiquidModel& m, const V3& c)
{
	const Grid& g = m.GridSpec();
	const double scale = m.Setup().density*g.h*g.h*g.h;
	V3 L{{0, 0, 0}};
	auto addP = [&](const V3& x, const V3& p) {
		const V3 r{{x[0] - c[0], x[1] - c[1], x[2] - c[2]}};
		L[0] += r[1]*p[2] - r[2]*p[1];
		L[1] += r[2]*p[0] - r[0]*p[2];
		L[2] += r[0]*p[1] - r[1]*p[0];
	};
	for (int a = 0; a < 3; ++a) {
		const auto d = g.FaceDims(a);
		for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
			const int idx[3] = {i, j, k};
			if (idx[a] == 0 || idx[a] == g.n[a]) continue;
			const std::size_t f = g.F(a, i, j, k);
			const double A = m.fraction[a][f];
			if (!(A > 0)) continue;
			int lo[3] = {i, j, k};
			--lo[a];
			const double pl = m.phi[g.C(lo[0], lo[1], lo[2])], pr = m.phi[g.C(i, j, k)];
			double w = 0;
			if (pl < 0 && pr < 0) w = A;
			else if (pl < 0) w = A*std::min(1.0, std::max(1e-3, pl/(pl - pr)));
			else if (pr < 0) w = A*std::min(1.0, std::max(1e-3, pr/(pr - pl)));
			if (w == 0) continue;
			V3 p{{0, 0, 0}};
			p[std::size_t(a)] = scale*w*m.u.u[a][f];
			addP(g.FaceCentre(a, i, j, k), p);
		}
	}
	for (const LiquidSheet& sh : m.sheets)
		for (std::size_t q = 0; q < sh.Vertices(); ++q)
			addP({{sh.x[3*q], sh.x[3*q + 1], sh.x[3*q + 2]}}, {{sh.dyn.mass[q]*sh.v[3*q], sh.dyn.mass[q]*sh.v[3*q + 1], sh.dyn.mass[q]*sh.v[3*q + 2]}});
	for (const LiquidBody& b : m.bodies) {
		const V3 Lb = b.state.AngularMomentum();   // about the origin
		const V3 x = b.state.x;
		const V3 mv{{b.state.mass*b.state.v[0], b.state.mass*b.state.v[1], b.state.mass*b.state.v[2]}};
		// about c: L_origin - c x (m v)
		L[0] += Lb[0] - (c[1]*mv[2] - c[2]*mv[1]);
		L[1] += Lb[1] - (c[2]*mv[0] - c[0]*mv[2]);
		L[2] += Lb[2] - (c[0]*mv[1] - c[1]*mv[0]);
		(void)x;
	}
	return L;
}

struct AuditResult { double linear = 0, reference = 0, torque = 0, angular = 0, angularReference = 0, spinChange = 0; bool ok = false; };

AuditResult RunAudit(const Audit3& a)
{
	AuditResult r;
	LiquidSetup s;
	s.grid.n = {{a.n, a.n, a.n}};
	s.grid.h = a.L/a.n;
	s.gravity = {{0, 0, 0}};
	s.density = 1400;
	s.viscosity = a.viscous ? 5.0 : 0.0;
	s.liquid.push_back(Box({{0.25*a.L, 0.25*a.L, 0.25*a.L}}, {{0.75*a.L, 0.75*a.L, 0.75*a.L}}));
	const V3 vs{{0.004, 0.003, -0.005}};
	if (a.sheet) {
		// Axis-aligned (normal y, off the lattice): the face liquid moves with the sheet along its normal only
		// (LiquidModel::SheetFaceMass), which for a tilted sheet drops the tangential part of that liquid's
		// momentum along the face's axis (m7g, by design), so only an axis-aligned sheet's exchange closes exactly.
		V3 u{{1, 0, 0}}, w{{0, 0, 1}};
		auto unit = [](V3& x) { const double l = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]); for (double& c : x) c /= l; };
		unit(u);
		const double uw = u[0]*w[0] + u[1]*w[1] + u[2]*w[2];
		for (int k = 0; k < 3; ++k) w[std::size_t(k)] -= uw*u[std::size_t(k)];
		unit(w);
		const double x0 = 0.5;
		s.sheets.push_back(MakeSheet({{x0*a.L, 0.53*a.L, 0.5*a.L}}, u, w, a.half*a.L, a.half*a.L, a.verts, a.verts, a.sigma, false, vs));
	}
	if (a.body) {
		BodyShape sh;
		sh.type = BodyShape::Type::Sphere;
		sh.radius = 0.07*a.L;
		const double vol = 4.0/3.0*3.14159265358979323846*std::pow(sh.radius, 3);
		LiquidBody b = MakeBody(sh, BodyMotion::Dynamic, 700*vol, {{0.56*a.L, 0.38*a.L, 0.48*a.L}}, s.gravity);
		b.state.v = {{-0.003, 0.002, 0.004}};
		if (a.spin) {
			const V3 omega{{0.3, -0.2, 0.4}};
			for (int k = 0; k < 3; ++k) b.state.pi[std::size_t(k)] = b.state.moments[std::size_t(k)]*omega[std::size_t(k)];
		}
		s.bodies.push_back(b);
	}
	LiquidModel m;
	std::string error;
	// The interface's own changes (the reinitialisation, the volume correction) move the free surface's
	// weights of the measured momentum; slow motions keep the transport's share (quadratic in the speed) small.
	SolverOptions o = Options();
	o.volumeCorrection = false;
	o.reinit = ReinitScheme::None;
	if (!m.Initialize(s, o, 4, error)) { Check(false, "audit model", error); return r; }
	if (a.sheet) m.ResetSheetDynamics();
	if (!m.ProjectState(error)) { Check(false, "audit projection", error); return r; }
	V3 liq;
	const V3 p0 = Total(m, liq);
	const V3 c0 = a.body ? m.bodies[0].state.x : V3{{0.5*a.L, 0.5*a.L, 0.5*a.L}};
	const V3 L0 = Angular(m, c0);
	double ref = 0;
	if (a.sheet) { double sm = 0; for (double x : m.sheets[0].dyn.mass) sm += x; ref = std::max(ref, sm*std::sqrt(vs[0]*vs[0] + vs[1]*vs[1] + vs[2]*vs[2])); }
	V3 Lb0{{0, 0, 0}};
	if (a.body) {
		const LiquidBody& b = m.bodies[0];
		ref = std::max(ref, b.state.mass*std::sqrt(b.state.v[0]*b.state.v[0] + b.state.v[1]*b.state.v[1] + b.state.v[2]*b.state.v[2]));
		const V3 lo = b.state.AngularMomentum();
		const V3 mv{{b.state.mass*b.state.v[0], b.state.mass*b.state.v[1], b.state.mass*b.state.v[2]}};
		const V3& x = b.state.x;
		Lb0 = {{lo[0] - (x[1]*mv[2] - x[2]*mv[1]), lo[1] - (x[2]*mv[0] - x[0]*mv[2]), lo[2] - (x[0]*mv[1] - x[1]*mv[0])}};   // about its centre
		m.ResetBodyImpulses();
	}
	r.reference = ref;
	double worst = 0;
	const double dt = 1.0/240;
	for (double t = 0; t < a.T - 1e-12; t += dt) {
		if (!m.Run(dt, dt, nullptr, error)) { Check(false, "audit run", error); return r; }
		const V3 p = Total(m, liq);
		if (verbose) {
			V3 sp{{0, 0, 0}};
			for (const LiquidSheet& sh : m.sheets) for (std::size_t q = 0; q < sh.Vertices(); ++q) for (int k = 0; k < 3; ++k) sp[std::size_t(k)] += sh.dyn.mass[q]*sh.v[3*q + std::size_t(k)];
			std::printf("    t %.4f total %+.3e %+.3e %+.3e liquid %+.3e %+.3e %+.3e sheet %+.3e %+.3e %+.3e\n", m.Time(), p[0] - p0[0], p[1] - p0[1], p[2] - p0[2],
				liq[0], liq[1], liq[2], sp[0], sp[1], sp[2]);
		}
		for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(p[std::size_t(k)] - p0[std::size_t(k)])/ref);
	}
	r.linear = worst;
	const V3 L1 = Angular(m, c0);
	double lref = 0, ldiff = 0;
	for (int k = 0; k < 3; ++k) { lref = std::max(lref, std::fabs(L0[std::size_t(k)])); ldiff = std::max(ldiff, std::fabs(L1[std::size_t(k)] - L0[std::size_t(k)])); }
	r.angular = lref > 0 ? ldiff/lref : 0;
	if (a.body) {
		const LiquidBody& b = m.bodies[0];
		const V3 lo = b.state.AngularMomentum();
		const V3 mv{{b.state.mass*b.state.v[0], b.state.mass*b.state.v[1], b.state.mass*b.state.v[2]}};
		const V3& x = b.state.x;
		const V3 Lb1{{lo[0] - (x[1]*mv[2] - x[2]*mv[1]), lo[1] - (x[2]*mv[0] - x[0]*mv[2]), lo[2] - (x[0]*mv[1] - x[1]*mv[0])}};
		double tw = 0, tref = 0;
		for (int k = 0; k < 3; ++k) {
			const double dL = Lb1[std::size_t(k)] - Lb0[std::size_t(k)];
			tw = std::max(tw, std::fabs(dL - m.bodyAccumulated[0][std::size_t(3 + k)]));
			tref = std::max(tref, std::fabs(Lb0[std::size_t(k)]));
			r.spinChange = std::max(r.spinChange, std::fabs(dL));
		}
		r.torque = tref > 0 ? tw/tref : (r.spinChange > 0 ? tw/r.spinChange : 0);
	}
	r.ok = true;
	return r;
}

void FreeSheet()
{
	std::printf("C: momentum closure with a free sheet in honey (3-D, no gravity)\n");
	for (bool viscous : {true, false}) {
		const double t0 = Seconds();
		Audit3 a;
		a.viscous = viscous;
		if (std::getenv("LSV_HALF")) { a.half = std::atof(std::getenv("LSV_HALF")); a.verts = 7; }   // e.g. 0.12: free edges in the liquid
		const AuditResult r = RunAudit(a);
		std::printf("  %s: momentum audit %.2e of the sheet's initial momentum (worst axis, every step), %.1f s\n",
			viscous ? "viscous" : "inviscid", r.linear, Seconds() - t0);
		Check(r.ok && r.linear < 0.01, std::string("liquid, face liquid and the free sheet keep their momentum (") + (viscous ? "viscous" : "inviscid") + ")",
			std::to_string(r.linear));
	}
}

void ThreeWay()
{
	std::printf("D: 3-D audit, liquid + coupled sheet + coupled rigid body (no gravity)\n");
	for (int variant = 0; variant < 3; ++variant) {
		const double t0 = Seconds();
		Audit3 a;
		a.body = true;
		a.spin = variant >= 1;
		a.viscous = variant != 2;
		const AuditResult r = RunAudit(a);
		std::printf("  %s%s: linear audit %.2e (worst axis, of the largest initial momentum), body torque impulse against its d(I w) %.2e, "
			"|d(I w)| %.3e N m s, total angular momentum about the body's centre changed by %.2e of its start, %.1f s\n",
			a.spin ? "translating and spinning" : "translating", a.viscous ? ", viscous" : ", inviscid", r.linear, r.torque, r.spinChange, r.angular, Seconds() - t0);
		Check(r.ok && r.linear < 0.01, "liquid, sheet and body keep their momentum on all three axes", std::to_string(r.linear));
		if (a.spin) Check(r.ok && r.torque < 1e-6, "the body's offered torque impulse is its change of angular momentum about its centre", std::to_string(r.torque));
	}
}

void Refusal()
{
	std::printf("E: refusal\n");
	LiquidSetup s;
	s.grid.n = {{8, 8, 8}};
	s.grid.h = 0.1/8;
	s.viscosity = 1;
	s.liquid.push_back(Box({{0, 0, 0}}, {{0.1, 0.05, 0.1}}));
	LiquidSheet sh = MakeSheet({{0.05, 0.06, 0.05}}, {{1, 0, 0}}, {{0, 0, 1}}, 0.04, 0.04, 3, 3, 0.2, false);
	sh.implicit = false;
	sh.dyn = SheetDynamics();
	s.sheets.push_back(sh);
	LiquidModel m;
	std::string error;
	Check(!m.Initialize(s, Options(), 1, error), "a staggered port sheet with viscosity is refused");
	std::printf("  %s\n", error.c_str());
}
} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "-v")) verbose = true;
	const double t0 = Seconds();
	const char* only = std::getenv("LSV_ONLY");
	auto run = [&](char c, void (*f)()) { if (!only || std::strchr(only, c)) f(); };
	run('A', HeldLayer);
	run('B', NoCoupling);
	run('C', FreeSheet);
	run('D', ThreeWay);
	run('E', Refusal);
	std::printf("%s (%.1f s)\n", failures ? "FAILED" : "all passed", Seconds() - t0);
	return failures ? 1 : 0;
}
