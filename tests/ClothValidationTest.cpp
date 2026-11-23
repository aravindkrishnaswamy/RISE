// Cloth model validation (simulation M3a, docs/SIMULATION_M3A_RESULTS.md).
//
//   element checks  membrane, hinge and rod gradients against finite
//                   differences; the membrane Hessian exact in tension and
//                   positive semidefinite after projection
//   references      elastica and elastic catenary against closed forms
//   case 1          hanging strip vs the elastic catenary
//   case 2          warp and weft cantilevers (different rigidities) vs the elastica;
//                   the discrete-shell hinge model shown not to converge
//   case 3          pretensioned membrane modes (1,1), (2,1), (1,2) vs f_mn,
//                   and against the fully discrete (space + integrator) prediction
//   case 4          simply supported and clamped square plates vs lambda^2 = 2 pi^2, 35.985
//   case 5          energy change per period of each integrator vs its linear theory
//   case 6          observed orders for cases 2, 3 (and 1, 4)
//
// Fast tier: three refinement levels per case.  --extended (or
// RISE_TEST_EXTENDED=1) adds finer levels.  Tolerances are 1.25 x the error
// observed at that level (rounded up), so a regression in accuracy fails;
// orders must reach 1.8 (second-order schemes).
#include "../src/Library/Simulation/Cloth/ClothValidation.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace RISE::Simulation::Cloth;
int failures = 0;
constexpr double Pi = 3.14159265358979323846;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}

double Seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Row(const char* id, const char* reference, const CaseResult& r, double tolerance, double seconds)
{
	const bool pass = r.ok && r.error <= tolerance;
	std::printf("| %-26s | %-28s | %4d | %10.3e | %9.2e | %-4s | %6.2f s |\n", id, reference, r.resolution, r.error, tolerance,
		pass ? "pass" : "FAIL", seconds);
	if (!r.detail.empty()) std::printf("|   %s\n", r.detail.c_str());
	Check(pass, std::string(id) + " at " + std::to_string(r.resolution), r.failure + " error " + std::to_string(r.error));
}

void OrderRow(const char* id, const std::vector<CaseResult>& rs, double minimum)
{
	std::printf("| order %-20s |", id);
	double worst = 10;
	for (std::size_t k = 0; k + 1 < rs.size(); ++k) {
		const double p = ObservedOrder(rs[k].error, rs[k+1].error);
		worst = std::min(worst, p);
		std::printf(" %d->%d: %.2f", rs[k].resolution, rs[k+1].resolution, p);
	}
	std::printf("  (minimum %.2f, required %.1f)\n", worst, minimum);
	Check(worst >= minimum, std::string("observed order of ") + id, std::to_string(worst));
}

Material Cotton()
{
	Material m;
	FindMaterial("cotton-plain-weave", m);
	return m;
}

// ---------------------------------------------------------------- element checks

void ElementChecks()
{
	std::string error;
	Material m = Cotton();
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> jitter(-1, 1);
	for (int model = 0; model < 2; ++model) {
		SheetSpec spec;
		spec.verticesU = 6; spec.verticesV = 5; spec.sizeU = 0.25; spec.sizeV = 0.2;
		spec.bending = model ? BendingModel::Hinges : BendingModel::Lines;
		spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 1, 0}};
		Sheet sheet;
		ClothSystem system;
		ModelConfig config;
		config.material = m;
		// Bending-only energy: a negligible membrane isolates the hinges and rods.
		config.material.stretchWarp = config.material.stretchWeft = 1e-9; config.material.shear = 1e-10; config.material.poisson = 0;
		Check(BuildSheet(spec, config.material, sheet, error) && system.Initialize(sheet, config, error), "bending check sheet", error);
		std::vector<double> x = system.x, dir(x.size());
		for (auto& c : x) c += 0.01*jitter(rng);
		for (auto& c : dir) c = jitter(rng);
		const double eps = 1e-6;
		std::vector<double> xp = x, xm = x, g;
		for (std::size_t k = 0; k < x.size(); ++k) { xp[k] += eps*dir[k]; xm[k] -= eps*dir[k]; }
		const double fd = (system.ElasticEnergy(xp) - system.ElasticEnergy(xm))/(2*eps);
		system.ElasticGradient(x, g);
		double an = 0;
		for (std::size_t k = 0; k < x.size(); ++k) an += g[k]*dir[k];
		std::printf("element: %s bending gradient vs finite difference: relative %.2e\n", model ? "hinge" : "rod+diagonal-hinge",
			std::fabs(fd - an)/std::fabs(an));
		Check(std::fabs(fd - an) <= 1e-6*std::fabs(an), "bending gradient matches finite differences");
	}
	// Membrane: gradient everywhere; Hessian exact in pure tension; projected Hessian PSD in compression.
	{
		SheetSpec spec;
		spec.verticesU = 5; spec.verticesV = 4; spec.sizeU = 0.2; spec.sizeV = 0.15;
		spec.axisU = {{1, 0, 0}}; spec.axisV = {{0, 1, 0}};
		ModelConfig config;
		config.material = m;
		config.material.bendWarp = config.material.bendWeft = 0;
		Sheet sheet;
		ClothSystem system;
		Check(BuildSheet(spec, config.material, sheet, error) && system.Initialize(sheet, config, error), "membrane sheet", error);
		for (int pass = 0; pass < 2; ++pass) {
			std::vector<double> x = system.x, dir(x.size()), g, gp, gm, hd;
			for (std::size_t k = 0; k < x.size(); ++k) {
				x[k] *= pass == 0 ? 1.01 : 0.97;        // stretched / compressed
				x[k] += (pass == 0 ? 1e-4 : 5e-3)*jitter(rng);
			}
			for (auto& c : dir) c = jitter(rng);
			const double eps = 1e-7;
			std::vector<double> xp = x, xm = x;
			for (std::size_t k = 0; k < x.size(); ++k) { xp[k] += eps*dir[k]; xm[k] -= eps*dir[k]; }
			system.ElasticGradient(x, g);
			system.ElasticGradient(xp, gp);
			system.ElasticGradient(xm, gm);
			BlockMatrix h;
			system.ElasticHessian(x, h);
			h.Multiply(dir, hd);
			double num = 0, den = 0, dirHd = 0;
			for (std::size_t k = 0; k < x.size(); ++k) {
				const double fdk = (gp[k] - gm[k])/(2*eps);
				num += (fdk - hd[k])*(fdk - hd[k]); den += fdk*fdk; dirHd += dir[k]*hd[k];
			}
			const double fd = (system.ElasticEnergy(xp) - system.ElasticEnergy(xm))/(2*eps);
			double an = 0;
			for (std::size_t k = 0; k < x.size(); ++k) an += g[k]*dir[k];
			Check(std::fabs(fd - an) <= 1e-6*std::fabs(an), "membrane gradient matches finite differences");
			if (pass == 0) {
				std::printf("element: membrane Hessian in tension vs finite differences: relative %.2e\n", std::sqrt(num/den));
				Check(std::sqrt(num/den) < 1e-6, "membrane Hessian is exact in tension");
			} else {
				// PSD: Rayleigh quotients over random directions stay non-negative.
				double worst = dirHd;
				for (int t = 0; t < 50; ++t) {
					for (auto& c : dir) c = jitter(rng);
					h.Multiply(dir, hd);
					double q = 0;
					for (std::size_t k = 0; k < x.size(); ++k) q += dir[k]*hd[k];
					worst = std::min(worst, q);
				}
				std::printf("element: projected membrane Hessian in compression: smallest of 51 Rayleigh quotients %.3e\n", worst);
				Check(worst >= 0, "projected membrane Hessian is positive semidefinite in compression");
			}
		}
	}
}

// ---------------------------------------------------------------- references

void References()
{
	std::string error;
	// Elastica: small load -> q L^4 / (8 B); large load is checked by the cases.
	std::vector<double> s{0.5, 1.0}, x, d;
	Check(ElasticaCantilever(1e-6, 1.0, 1.0, 0, s, x, d, error) && std::fabs(d[1] - 1e-6/8)/(1e-6/8) < 1e-5,
		"elastica small-load limit", std::to_string(d[1]));
	// Elastic catenary: stiff, Hookean limit vs the inextensible catenary (closed form).
	const double L0 = 1, span = 0.8, w0 = 1;
	double H = 0;
	std::vector<double> sc{0.5}, xc, zc;
	Check(ElasticCatenary(L0, span, w0, 1e12, sc, xc, zc, H, error), "elastic catenary", error);
	// Inextensible: span = 2 (H/w) asinh(w L0 / (2H)); sag = (H/w)(cosh(w span/(2H)) - 1).
	double lo = 1e-3, hi = 1e3;
	for (int it = 0; it < 200; ++it) {
		const double mid = 0.5*(lo + hi);
		if (2*mid/w0*std::asinh(w0*L0/(2*mid)) < span) lo = mid; else hi = mid;
	}
	const double Hc = 0.5*(lo + hi), sag = Hc/w0*(std::cosh(w0*span/(2*Hc)) - 1);
	std::printf("reference: catenary H %.12f vs closed form %.12f; midspan sag %.12f vs %.12f\n", H, Hc, zc[0], sag);
	Check(std::fabs(H - Hc)/Hc < 1e-9 && std::fabs(zc[0] - sag)/sag < 1e-9, "elastic catenary reduces to the catenary");
	// Irvine's (1981) closed-form Hookean elastic catenary at EA = 1000 w0 L0 (strain ~1e-3):
	// the StVK law T = EA l (l^2 - 1)/2 differs from Hooke's at O(strain^2).
	const double EA = 1000*w0*L0, va = 0.5*w0*L0;
	auto irvineSpan = [&](double h) { return h*L0/EA + 2*h/w0*std::asinh(va/h); };
	lo = 1e-3; hi = 1e3;
	for (int it = 0; it < 200; ++it) {
		const double mid = 0.5*(lo + hi);
		if (irvineSpan(mid) < span) lo = mid; else hi = mid;
	}
	const double Hi = 0.5*(lo + hi), s2 = 0.5*L0;
	const double sagIrvine = w0*s2/EA*(va/w0 - s2/2) + (std::sqrt(Hi*Hi + va*va) - std::sqrt(Hi*Hi + (va - w0*s2)*(va - w0*s2)))/w0;
	Check(ElasticCatenary(L0, span, w0, EA, sc, xc, zc, H, error), "elastic catenary", error);
	std::printf("reference: StVK elastic catenary sag %.10f vs Irvine (Hookean) %.10f; relative %.2e\n", zc[0], sagIrvine,
		std::fabs(zc[0] - sagIrvine)/sagIrvine);
	Check(std::fabs(zc[0] - sagIrvine)/sagIrvine < 1e-4 && zc[0] > sag, "elastic catenary agrees with Irvine's solution to O(strain^2)");
}

// ---------------------------------------------------------------- cases

std::vector<int> Levels(std::initializer_list<int> fast, std::initializer_list<int> extra, bool extended)
{
	std::vector<int> v(fast);
	if (extended) v.insert(v.end(), extra);
	return v;
}

double Tolerance(const std::vector<std::pair<int,double>>& table, int resolution)
{
	for (const auto& t : table) if (t.first == resolution) return t.second;
	return 0;
}

void Header(const char* title)
{
	std::printf("\n%s\n| %-26s | %-28s | %4s | %10s | %9s | %-4s | %8s |\n", title, "case", "reference", "res", "error",
		"tolerance", "", "time");
}

void Catenary(bool extended)
{
	Header("Case 1: hanging strip (no bending) vs the elastic catenary; error = max position error / sag");
	// Observed: 5.372e-3, 1.332e-3, 3.323e-4, 8.291e-5, 2.067e-5 (tolerance 1.25x, rounded up).
	const std::vector<std::pair<int,double>> tol{{10, 6.8e-3}, {20, 1.7e-3}, {40, 4.2e-4}, {80, 1.1e-4}, {160, 2.6e-5}};
	std::vector<CaseResult> rs;
	CaseSettings s;
	s.material = Cotton();
	for (int n : Levels({10, 20, 40}, {80, 160}, extended)) {
		s.resolution = n;
		const double t0 = Seconds();
		rs.push_back(RunCatenary(s));
		Row("catenary", "elastic catenary", rs.back(), Tolerance(tol, n), Seconds() - t0);
	}
	OrderRow("catenary", rs, 1.8);
}

void Cantilever(bool extended)
{
	Header("Case 2: cantilever under gravity vs the elastica (L = 2 bending lengths); error = max position error / tip drop");
	// Observed (lines model, either triangulation, warp B = 2e-5 and weft B = 1.2e-5 N m, same
	// geometry in bending lengths): 3.355e-3, 6.885e-4, 1.771e-4, 4.754e-5, 1.238e-5.
	const std::vector<std::pair<int,double>> tol{{10, 4.2e-3}, {20, 8.7e-4}, {40, 2.3e-4}, {80, 6.0e-5}, {160, 1.6e-5}};
	CaseSettings s;
	s.material = Cotton();
	std::vector<CaseResult> warp, weft;
	for (int n : Levels({10, 20, 40}, {80, 160}, extended)) {
		s.resolution = n;
		double t0 = Seconds();
		warp.push_back(RunCantilever(s, false));
		Row("cantilever warp", "elastica, B_warp", warp.back(), Tolerance(tol, n), Seconds() - t0);
		t0 = Seconds();
		weft.push_back(RunCantilever(s, true));
		Row("cantilever weft", "elastica, B_weft", weft.back(), Tolerance(tol, n), Seconds() - t0);
	}
	OrderRow("cantilever warp", warp, 1.8);
	OrderRow("cantilever weft", weft, 1.8);
	// Same on the consistent triangulation (the line model does not depend on it).
	s.triangulation = Triangulation::Consistent;
	s.resolution = 20;
	const CaseResult consistent = RunCantilever(s, false);
	Row("cantilever warp consistent", "elastica, B_warp", consistent, Tolerance(tol, 20), 0);
	// The discrete-shell hinge model: documented as not converging on either structured
	// triangulation (a grid-scale mode between vertex rows softens it).
	std::printf("| hinge bending model (documented failure; not a gate on the shipped model):\n");
	s.bending = BendingModel::Hinges;
	for (int t = 0; t < 2; ++t) {
		s.triangulation = t ? Triangulation::Consistent : Triangulation::Alternating;
		std::vector<CaseResult> hinge;
		for (int n : {10, 20, 40}) {
			s.resolution = n;
			hinge.push_back(RunCantilever(s, false));
			std::printf("|   hinges %-11s n=%2d: error %.3e, tip %.4e m vs %.4e m\n", TriangulationName(s.triangulation), n,
				hinge.back().error, hinge.back().measured, hinge.back().reference);
		}
		Check(hinge.back().ok && hinge.back().error > 0.1 && hinge.back().error >= hinge.front().error,
			"the hinge model's cantilever error does not decrease under refinement (documented)");
	}
}

// Fully discrete frequency: the integrator's principal root at the semi-discrete omega.
double DiscreteFrequency(Scheme scheme, double omega, double dt)
{
	const std::complex<double> z(0, omega*dt);
	std::complex<double> zeta;
	if (scheme == Scheme::BackwardEuler) zeta = 1.0/(1.0 - z);
	else if (scheme == Scheme::BDF2) zeta = (2.0 + std::sqrt(1.0 + 2.0*z))/(3.0 - 2.0*z);
	else zeta = (1.0 + 0.5*z)/(1.0 - 0.5*z);
	return std::arg(zeta)/(2*Pi*dt);
}

void Membrane(bool extended)
{
	Header("Case 3: pretensioned membrane (a = 1.003 m, b = 0.752 m, 0.3% stretch), free modes, BDF2, 2.5 n steps/period");
	// Observed (BDF2; time and space both O(h^2)): (1,1) 3.771e-2, 1.021e-2, 2.608e-3, 6.535e-4;
	// (2,1) 4.961e-2, 1.339e-2, 3.416e-3, 8.532e-4; (1,2) 6.476e-2, 1.748e-2, 4.458e-3, 1.112e-3.
	const std::vector<std::pair<int,double>> tol11{{8, 4.8e-2}, {16, 1.3e-2}, {32, 3.3e-3}, {64, 8.2e-4}};
	const std::vector<std::pair<int,double>> tol21{{8, 6.3e-2}, {16, 1.7e-2}, {32, 4.3e-3}, {64, 1.1e-3}};
	const std::vector<std::pair<int,double>> tol12{{8, 8.1e-2}, {16, 2.2e-2}, {32, 5.6e-3}, {64, 1.4e-3}};
	CaseSettings s;
	s.material = Cotton();
	const int modes[3][2] = {{1, 1}, {2, 1}, {1, 2}};
	for (int k = 0; k < 3; ++k) {
		std::vector<CaseResult> rs;
		const auto& tol = k == 0 ? tol11 : k == 1 ? tol21 : tol12;
		const std::string id = "membrane (" + std::to_string(modes[k][0]) + "," + std::to_string(modes[k][1]) + ")";
		for (int n : Levels({8, 16, 32}, {64}, extended)) {
			s.resolution = n;
			const double t0 = Seconds();
			rs.push_back(RunMembraneMode(s, modes[k][0], modes[k][1]));
			Row(id.c_str(), "f_mn (tension, density)", rs.back(), Tolerance(tol, n), Seconds() - t0);
			// Cross-check: the measured frequency is the fully discrete one (semi-discrete
			// omega_h through the integrator's principal root) to within 2e-4.
			const double dt = 1/(rs.back().reference*2.5*n);
			const double predicted = DiscreteFrequency(s.scheme, 2*Pi*rs.back().secondary, dt);
			const double gap = std::fabs(rs.back().measured - predicted)/predicted;
			std::printf("|   fully discrete prediction %.6f Hz, measured %.6f Hz: relative gap %.2e\n", predicted, rs.back().measured, gap);
			Check(gap < 2e-4, id + " matches its fully discrete prediction", std::to_string(gap));
		}
		OrderRow(id.c_str(), rs, 1.8);
	}
}

void Plate(bool extended)
{
	Header("Case 4: square plate, a = 0.2 m, B = 2e-5 N m (isotropic, D = B), fundamental mode, BDF2, 2.5 n steps/period");
	// Observed: SS 4.103e-2, 1.110e-2, 2.835e-3, 7.433e-4; clamped 8.985e-2, 2.567e-2, 6.674e-3, 1.684e-3.
	const std::vector<std::pair<int,double>> tolSS{{8, 5.2e-2}, {16, 1.4e-2}, {32, 3.6e-3}, {64, 9.3e-4}};
	const std::vector<std::pair<int,double>> tolC{{8, 1.13e-1}, {16, 3.3e-2}, {32, 8.4e-3}, {64, 2.1e-3}};
	Material iso = Cotton();
	iso.bendWeft = iso.bendWarp;
	CaseSettings s;
	s.material = iso;
	for (int clamped = 0; clamped < 2; ++clamped) {
		std::vector<CaseResult> rs;
		for (int n : Levels({8, 16, 32}, {64}, extended)) {
			s.resolution = n;
			const double t0 = Seconds();
			rs.push_back(RunPlate(s, clamped != 0));
			Row(clamped ? "plate clamped" : "plate simply supported", clamped ? "35.985/a^2 sqrt(D/sigma)" : "2 pi^2/a^2 sqrt(D/sigma)",
				rs.back(), Tolerance(clamped ? tolC : tolSS, n), Seconds() - t0);
		}
		OrderRow(clamped ? "plate clamped" : "plate simply supported", rs, 1.75);
	}
	// Orthotropic simply supported plate: reference (pi/a)^4 (sqrt(D11) + sqrt(D22))^2 / sigma, exact for D66 = sqrt(D11 D22)/2, D12 = 0.
	s.material = Cotton();
	s.resolution = 16;
	const CaseResult ortho = RunPlate(s, false);
	Row("plate SS orthotropic", "(pi/a)^2 (sqrt D11+sqrt D22)", ortho, 1.4e-2, 0);
}

void Energy()
{
	Header("Case 5: undamped membrane (1,1) mode, 4 periods; error = |energy loss per period - linear theory|");
	// Observed (cotton): BE 2.877e-7, 7.096e-7; BDF2 1.372e-7, 1.968e-8; midpoint 5.129e-10,
	// 2.689e-11 (the midpoint values are at the Newton tolerance, 1e-9 of the oscillation scale).
	const double tol[3][2] = {{3.6e-7, 8.9e-7}, {1.8e-7, 2.5e-8}, {6.5e-10, 3.4e-11}};
	CaseSettings s;
	s.material = Cotton();
	for (int scheme = 0; scheme < 3; ++scheme) {
		s.scheme = static_cast<Scheme>(scheme);
		for (int level = 0; level < 2; ++level) {
			s.resolution = level ? 32 : 16;
			const double t0 = Seconds();
			const CaseResult r = RunEnergyDrift(s);
			Row((std::string("energy ") + SchemeName(s.scheme)).c_str(), "|zeta(i w dt)|^(2N)", r, tol[scheme][level], Seconds() - t0);
		}
	}
}

// The evidence resolutions for every built-in record and integrator: the
// errors the solver's evidence tolerances are derived from (1.25 x the
// largest, rounded up; ClothSolver.cpp).  Static cases do not depend on the integrator.
void EvidenceFamily()
{
	std::printf("\nEvidence resolutions over the built-in records (the solver's tolerances are 1.25 x the largest):\n");
	double worst[3] = {0, 0, 0}, dyn[3][4] = {{0}};
	for (const auto& m : BuiltInMaterials()) {
		CaseSettings s;
		s.material = m;
		s.resolution = 20;
		const CaseResult cat = RunCatenary(s), cw = RunCantilever(s, false), cf = RunCantilever(s, true);
		std::printf("  %-20s catenary %.3e  warp %.3e  weft %.3e\n", m.name.c_str(), cat.error, cw.error, cf.error);
		worst[0] = std::max(worst[0], cat.error); worst[1] = std::max(worst[1], cw.error); worst[2] = std::max(worst[2], cf.error);
		Check(cat.ok && cw.ok && cf.ok, "static evidence cases run for " + m.name);
		s.resolution = 16;
		for (int k = 0; k < 3; ++k) {
			s.scheme = static_cast<Scheme>(k);
			const CaseResult m11 = RunMembraneMode(s, 1, 1), m21 = RunMembraneMode(s, 2, 1), plate = RunPlate(s, false), e = RunEnergyDrift(s);
			const double v[4] = {m11.error, m21.error, plate.error, e.error};
			std::printf("  %-20s %-17s m11 %.3e  m21 %.3e  plate %.3e  energy %.2e\n", "", SchemeName(s.scheme), v[0], v[1], v[2], v[3]);
			for (int c = 0; c < 4; ++c) dyn[k][c] = std::max(dyn[k][c], v[c]);
			Check(m11.ok && m21.ok && plate.ok && e.ok, "dynamic evidence cases run for " + m.name);
		}
	}
	std::printf("  largest: catenary %.3e  warp %.3e  weft %.3e\n", worst[0], worst[1], worst[2]);
	for (int k = 0; k < 3; ++k)
		std::printf("  largest %-17s m11 %.3e  m21 %.3e  plate %.3e  energy %.2e\n", SchemeName(static_cast<Scheme>(k)),
			dyn[k][0], dyn[k][1], dyn[k][2], dyn[k][3]);
}
} // namespace

int main(int argc, char** argv)
{
	const bool extended = (argc == 2 && std::strcmp(argv[1], "--extended") == 0) ||
		(std::getenv("RISE_TEST_EXTENDED") && std::strcmp(std::getenv("RISE_TEST_EXTENDED"), "1") == 0);
	const double t0 = Seconds();
	std::printf("Cloth validation (%s)\n", extended ? "extended" : "fast");
	ElementChecks();
	References();
	Catenary(extended);
	Cantilever(extended);
	Membrane(extended);
	Plate(extended);
	Energy();
	EvidenceFamily();
	std::printf("\ntotal %.1f s\n", Seconds() - t0);
	if (failures) { std::cerr << failures << " cloth validation check(s) failed\n"; return 1; }
	std::cout << "Cloth validation passed\n";
	return 0;
}
