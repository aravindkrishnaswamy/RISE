// M7 prerequisites: rigid mass properties over a port and the owned implicit
// exchange of the group host (GroupPolicy::ownedExchanges,
// Capabilities::ownedImplicitPorts).  A rigid body (rigid.bodies) in a 1-D
// added-mass "fluid" (tests/SimulationAddedMassSolver.h):
//   * rigid.bodies offers body_mass_properties (mass, inertia, centre of mass),
//     constant, from the start time;
//   * staggered: stable but first order while the added mass is below the
//     body's mass, divergent above it (ratio 2: the error grows without bound);
//   * owned implicit: the fluid solves body and fluid jointly; stable at
//     added-mass ratios 2 and 10 and second order in the macro step;
//   * labels: an owned exchange declared by an owner with predictive evidence
//     is predictive; undeclared, or staggered, it is preview with a reason;
//   * configuration refusals and the group identity rule (no owned exchange:
//     unchanged identity).
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationTransform.h"
#include "../src/Library/Simulation/Rigid/RigidSolver.h"
#include "SimulationAddedMassSolver.h"

#include <cmath>
#include <cstdio>
#include <iostream>

namespace {
using namespace RISE::Simulation;
using namespace RISE::Simulation::Reference;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}

const double MS = 1.0, K = 10.0, G = 9.81, Y0 = 0.1;

V Fluid(const char* mode, double ma, double H, std::uint64_t frames, bool declare = true)
{
	return V::MapValue({{"added_mass", V::Float(ma)}, {"declare", V::Unsigned(declare ? 1 : 0)}, {"frames", V::Unsigned(frames)},
		{"gravity", V::Float(G)}, {"interval", V::Float(H)}, {"mode", V::String(mode)}, {"stiffness", V::Float(K)}});
}

V Body(double H, std::uint64_t frames)
{
	const auto vec = [](double x, double y, double z) { return V::ArrayValue({V::Float(x), V::Float(y), V::Float(z)}); };
	return V::MapValue({{"bodies", V::ArrayValue({V::MapValue({{"name", V::String("body")}, {"shape", V::String("sphere")},
		{"radius", V::Float(0.1)}, {"mass", V::Float(MS)}, {"position", vec(0, Y0, 0)}})})},
		{"frame_rate", V::Float(1/H)}, {"frames", V::Unsigned(frames)}, {"gravity", vec(0, -G, 0)}, {"substeps", V::Unsigned(8)}});
}

std::unique_ptr<ISimulationSolver> Make(const char* id)
{
	std::string error;
	auto s = CreateSolver(id, error);
	Check(s != nullptr, std::string("create ") + id, error);
	return s;
}

struct Run {
	std::unique_ptr<SimulationGroup> group;
	bool ok = false;
	std::string error;
};

Run Build(const char* mode, double ma, double H, double T, bool owned, bool declare = true)
{
	Run r;
	r.group = std::make_unique<SimulationGroup>();
	const auto frames = static_cast<std::uint64_t>(std::llround(T/H)) + 1;
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"fluid", "body"};
	if (owned) p.ownedExchanges = {{"fluid", {"body"}}};
	auto& g = *r.group;
	r.ok = g.AddMember("fluid", Make(AddedMassSolver::Id), Fluid(mode, ma, H, frames, declare), r.error) &&
		g.AddMember("body", Make(RigidSolverId), Body(H, frames), r.error) &&
		g.Connect({"body", "body_state", "fluid", "body_state"}, r.error) &&
		g.Connect({"body", "body_mass_properties", "fluid", "body_mass_properties"}, r.error) &&
		g.Connect({"fluid", "body_load", "body", "body_load"}, r.error) &&
		g.Configure(p, EmptySceneInputs(), r.error) && g.Start(r.error);
	return r;
}

//! Largest displacement error against the exact solution over the run, and the final one.
bool Errors(const char* mode, double ma, double H, double T, bool owned, double& worst, double& last)
{
	auto r = Build(mode, ma, H, T, owned);
	if (!r.ok) { Check(false, "build the added-mass group", r.error); return false; }
	worst = 0;
	last = 0;
	r.group->SetStepObserver([&](double t, const SimulationGroup& g) {
		const auto s = g.Snapshot("body");
		std::shared_ptr<const Bytes> bytes;
		std::string e;
		CacheEntry entry;
		const auto index = static_cast<std::int64_t>(std::llround(t/H));
		if (!s->FindEntry(index, "bodies", entry) || s->Read(index, "bodies", CodecBudget, bytes, e) != FrameStatus::Ready) return;
		TransformFrame f;
		if (!DecodeTransforms(*bytes, CodecBudget, f, e)) return;
		double ye, ve;
		AddedMassExact(MS, ma, K, G, Y0, 0, t, ye, ve);
		const double err = std::fabs(f.bodies[0].position[1] - ye);
		worst = std::isfinite(err) ? std::max(worst, err) : INFINITY;
		last = err;
	});
	std::string error;
	while (!r.group->Finished())
		if (!r.group->StepMacro(error)) {
			// A divergent staggered run ends with nonfinite samples (refused by
			// the codecs): that is the divergence, not a test failure.
			worst = INFINITY;
			return false;
		}
	return true;
}
}

int main()
{
	std::string error;
	Check(RegisterRigidSolver(error), "register rigid.bodies", error);
	RegisterAddedMassSolver();
	const double amplitude = std::fabs(Y0 + MS*G/K);   // distance from equilibrium

	// ---------------------------------------------------------------- the mass-properties port
	{
		auto body = Make(RigidSolverId);
		SolverDescription d;
		Check(body->Configure(Body(0.05, 3), EmptySceneInputs(), d, error) && body->Start(error), "configure the body", error);
		const PortDeclaration* mass = nullptr;
		for (const auto& p : d.ports) if (p.name == "body_mass_properties") mass = &p;
		Check(mass && mass->offers && mass->codec == "field.v1" && mass->output.empty() && mass->domain == TransformBodyDomain({"body"}) &&
			mass->channels.size() == 3 && mass->channels[0].name == "center_of_mass" && mass->channels[1].components == 9 &&
			mass->channels[2].name == "mass" && mass->channels[2].units == "kg", "rigid.bodies declares body_mass_properties");
		PortRelay relay;
		class Null final : public IFrameSink {
		public:
			bool Publish(std::int64_t, const std::string&, double, std::shared_ptr<const Bytes>, const Bytes&, std::string&) override { return true; }
		} sink;
		AdvanceRequest r;
		r.mode = AdvanceRequest::Mode::MacroStep;
		r.macroEnd = 0;
		AdvanceResult result;
		Check(body->Advance(r, sink, &relay, nullptr, result, error), "first advance", error);
		std::shared_ptr<const Bytes> bytes;
		FieldFrame f;
		relay.Connect("m", "body_mass_properties");
		Check(relay.Accept("m", 1.0, bytes, error) == PortStatus::Ready && DecodeField(*bytes, CodecBudget, f, error),
			"the mass properties are offered at the start time (and hold)", error);
		const auto f32 = [](std::uint32_t b) { float x; std::memcpy(&x, &b, 4); return double(x); };
		const double I = 0.4*MS*0.1*0.1;   // solid sphere
		Check(f.count == 1 && f32(f.channels[2].values[0]) == MS && std::fabs(f32(f.channels[1].values[0]) - I) < 1e-7 &&
			f32(f.channels[1].values[1]) == 0 && f32(f.channels[1].values[4]) == f32(f.channels[1].values[0]) &&
			f32(f.channels[0].values[0]) == 0, "mass, body-frame inertia (diagonal) and centre of mass",
			std::to_string(f32(f.channels[1].values[0])));
	}

	// ---------------------------------------------------------------- staggered vs owned implicit
	const double T = 2.0;
	{
		double worst = 0, last = 0;
		Errors("staggered", 0.5*MS, 0.02, T, false, worst, last);
		std::printf("staggered, added mass 0.5 m: max error %.3e (amplitude %.3f)\n", worst, amplitude);
		Check(std::isfinite(worst) && worst < amplitude, "staggered, ratio 0.5: stable");
		double half = 0, ignored = 0;
		Errors("staggered", 0.5*MS, 0.01, T, false, half, ignored);
		std::printf("staggered, added mass 0.5 m: max error %.3e at H = 0.01, observed order %.2f\n", half, std::log2(worst/half));
		Check(std::fabs(std::log2(worst/half) - 1) < 0.3, "staggered, ratio 0.5: first order");
		for (const double H : {0.02, 0.005}) {
			Errors("staggered", 2.0*MS, H, T, false, worst, last);
			std::printf("staggered, added mass 2 m, H %.3f: max error %.3e\n", H, worst);
			Check(!(worst < 1e3*amplitude), "staggered, ratio 2: diverges, and refining the step does not help");
		}
	}
	{
		for (const double ratio : {2.0, 10.0}) {
			double e[3], lastE[3];
			const double Hs[3] = {0.04, 0.02, 0.01};
			for (int i = 0; i < 3; ++i) Errors("owner", ratio*MS, Hs[i], T, true, e[i], lastE[i]);
			const double p1 = std::log2(e[0]/e[1]), p2 = std::log2(e[1]/e[2]);
			std::printf("owned implicit, added mass %g m: max error %.3e / %.3e / %.3e at H = 0.04 / 0.02 / 0.01, observed order %.2f, %.2f\n",
				ratio, e[0], e[1], e[2], p1, p2);
			Check(std::isfinite(e[0]) && e[0] < 0.1*amplitude, "owned implicit: stable where staggered diverges");
			Check(std::fabs(p1 - 2) < 0.25 && std::fabs(p2 - 2) < 0.25, "owned implicit: second order in the macro step");
		}
	}

	// ---------------------------------------------------------------- labels and refusals
	{
		auto owned = Build("owner", 2.0*MS, 0.02, 0.2, true);
		Check(owned.ok, "owned group", owned.error);
		std::printf("owned: body %s (%s), fluid %s (%s)\n", owned.group->MemberFidelity("body").c_str(),
			owned.group->CouplingLimit("body").c_str(), owned.group->MemberFidelity("fluid").c_str(),
			owned.group->CouplingLimit("fluid").c_str());
		Check(owned.ok && owned.group->MemberFidelity("body") == "predictive" && owned.group->CouplingLimit("body").empty() &&
			owned.group->MemberFidelity("fluid") == "predictive", "a declared owned exchange with predictive members is predictive");
		const auto* d = owned.group->Description("body");
		bool condition = false;
		for (const auto& c : d->evidence.conditions) if (c.first == "coupling" && c.second == std::vector<std::string>{"owned-implicit"}) condition = true;
		Check(condition, "the coupling condition names the owned implicit exchange");

		auto undeclared = Build("owner", 2.0*MS, 0.02, 0.2, true, false);
		Check(undeclared.ok && undeclared.group->MemberFidelity("body") == "preview" &&
			undeclared.group->CouplingLimit("body").find("not declared") != std::string::npos,
			"an owner that does not declare its ports gives preview", undeclared.group ? undeclared.group->CouplingLimit("body") : "");
		auto staggered = Build("staggered", 0.5*MS, 0.02, 0.2, false);
		Check(staggered.ok && staggered.group->MemberFidelity("body") == "preview" &&
			staggered.group->CouplingLimit("body").find("staggered") != std::string::npos, "staggered coupling is preview");

		// Refusals: Jacobi; the owner after its partner; a partner that is not a member.
		const auto refused = [&](GroupPolicy p) {
			SimulationGroup g;
			std::string e;
			const bool ok = g.AddMember("fluid", Make(AddedMassSolver::Id), Fluid("owner", 1, 0.05, 5), e) &&
				g.AddMember("body", Make(RigidSolverId), Body(0.05, 5), e) &&
				g.Connect({"body", "body_state", "fluid", "body_state"}, e) &&
				g.Connect({"body", "body_mass_properties", "fluid", "body_mass_properties"}, e) &&
				g.Connect({"fluid", "body_load", "body", "body_load"}, e) && g.Configure(p, EmptySceneInputs(), e);
			return !ok;
		};
		GroupPolicy jacobi; jacobi.order = ExchangeOrder::Jacobi; jacobi.ownedExchanges = {{"fluid", {"body"}}};
		Check(refused(jacobi), "an owned exchange under Jacobi order is refused");
		GroupPolicy late; late.memberOrder = {"body", "fluid"}; late.ownedExchanges = {{"fluid", {"body"}}};
		Check(refused(late), "an owner that steps after its partner is refused");
		GroupPolicy stranger; stranger.memberOrder = {"fluid", "body"}; stranger.ownedExchanges = {{"fluid", {"nobody"}}};
		Check(refused(stranger), "an unknown partner is refused");

		// Identity: the owned exchange enters the group identity; without one it is unchanged.
		auto plain = Build("owner", 2.0*MS, 0.02, 0.2, false);
		Check(plain.ok && plain.group->GroupIdentity() != owned.group->GroupIdentity(), "the owned exchange enters the group identity");
	}

	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("SimulationOwnedCouplingTest: all checks passed\n");
	return 0;
}
