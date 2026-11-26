// M7 prerequisites: a test "fluid" for the owned implicit exchange.  A rigid
// body (rigid.bodies, gravity along -y) moves in a 1-D column of
// incompressible liquid that adds an inertia `added_mass` and a restoring
// stiffness `stiffness` (buoyancy).  The exact motion is the oscillator
//     (m + ma) y'' = -k y - m g.
//   * mode "staggered": the fluid knows nothing of the body's inertia.  It
//     reads the body state at each macro-step start and estimates the
//     acceleration from the last two states, offering F = -ma a_est - k y.
//     The body then integrates under F: the classic partitioned scheme, whose
//     error amplification is -ma/m -- unstable once ma > m whatever the step
//     (Causin, Gerbeau and Nobile 2005).
//   * mode "owner": the fluid reads the body's mass over body_mass_properties
//     and solves the JOINT system over the macro step (Crank-Nicolson: second
//     order, A-stable), then offers the step-average of its own hydrodynamic
//     force.  Held constant over the step by the rigid solver, that force
//     reproduces the joint solve's step exactly (velocity: same impulse;
//     position: the same parabola), so the coupled run is the joint scheme.
//     It declares body_state, body_mass_properties and body_load as
//     Capabilities::ownedImplicitPorts (unless `declare` is false) and carries
//     evidence (observed order and error of its joint solve on a canonical
//     problem).
#ifndef RISE_TEST_SIMULATION_ADDED_MASS_SOLVER_H
#define RISE_TEST_SIMULATION_ADDED_MASS_SOLVER_H

#include "SimulationReferenceSolvers.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationTransform.h"

namespace RISE { namespace Simulation { namespace Reference {

//! Exact displacement of (m + ma) y'' = -k y - m g from (y0, v0) after t.
inline void AddedMassExact(double m, double ma, double k, double g, double y0, double v0, double t, double& y, double& v)
{
	const double w = std::sqrt(k/(m + ma)), yeq = -m*g/k;
	y = yeq + (y0 - yeq)*std::cos(w*t) + v0/w*std::sin(w*t);
	v = -(y0 - yeq)*w*std::sin(w*t) + v0*std::cos(w*t);
}

//! One Crank-Nicolson step of the joint system over h: (y, v) in place, and
//! the step-average of the fluid force F = -ma y'' - k y (trapezoid rule).
inline double AddedMassJointStep(double m, double ma, double k, double g, double h, double& y, double& v)
{
	const double M = m + ma;
	const auto acc = [&](double yy) { return (-k*yy - m*g)/M; };
	// y1 = y + h (v + v1)/2, v1 = v + h (a(y) + a(y1))/2, linear: solve for y1.
	const double a0 = acc(y);
	// v1 = v + h/2 (a0 + (-k y1 - m g)/M); y1 = y + h/2 (v + v1)
	// => y1 = y + h v + h^2/4 (a0 + (-k y1 - m g)/M)
	const double y1 = (y + h*v + h*h/4*(a0 - m*g/M))/(1 + h*h*k/(4*M));
	const double a1 = acc(y1);
	const double v1 = v + h/2*(a0 + a1);
	const double F = 0.5*((-ma*a0 - k*y) + (-ma*a1 - k*y1));
	y = y1; v = v1;
	return F;
}

class AddedMassSolver final : public ISimulationSolver {
public:
	static constexpr const char* Id = "test.added_mass";
	bool Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error) override {
		configured_ = started_ = false;
		Params p(parameters, error);
		mode_ = p.Text("mode", "owner");
		ma_ = p.Real("added_mass", 1);
		k_ = p.Real("stiffness", 10);
		g_ = p.Real("gravity", 9.81);
		count_ = p.Count("frames", 41, 1, 1000000);
		interval_ = p.Real("interval", 0.05);
		const std::uint64_t declare = p.Count("declare", 1, 0, 1);
		SolverDescription c;
		if (!p.Finish(c.effectiveParameters)) return false;
		if ((mode_ != "owner" && mode_ != "staggered") || !(ma_ >= 0) || !(k_ > 0) || !(interval_ > 0))
			return Fail(error, "invalid added-mass parameters");
		c.solverId = Id; c.version = "1";
		c.cache.count = count_; c.cache.interval = interval_;
		c.cache.outputs = {{"load", "field.v1", {{"force", "N", "f32x3"}}}};
		c.ports = {
			{"body_state", "rigid body state", "m", TransformCodec, false, "",
				{{"angular_velocity", "angular velocity", "rad/s", 3}, {"orientation", "orientation", "1", 4},
				 {"position", "position", "m", 3}, {"velocity", "velocity", "m/s", 3}}, ""},
			{"body_mass_properties", "rigid body mass properties", "kg", "field.v1", false, "",
				{{"center_of_mass", "position", "m", 3}, {"inertia", "moment of inertia", "kg m^2", 9},
				 {"mass", "mass", "kg", 1}}, ""},
			{"body_load", "body load", "N", "field.v1", true, "load",
				{{"force", "force", "N", 3}, {"point", "position", "m", 3}, {"torque", "torque", "N m", 3}}, ""}};
		auto& caps = c.capabilities;
		caps.backends = {{"cpu", BackendStatus::Supported, true, "test"}};
		caps.interactive = caps.deterministic = caps.resumable = caps.externalMacroStep = true;
		caps.threadCountInvariant = true;
		if (mode_ == "owner") {
			if (declare) caps.ownedImplicitPorts = {"body_load", "body_mass_properties", "body_state"};
			caps.earnableFidelity = {"preview", "predictive"};
			c.evidence.fidelity = "predictive";
			c.evidence.conditions = {{"mode", {"owner"}}};
			c.evidence.scope = "The joint body-fluid solve of a 1-D added-mass oscillator by Crank-Nicolson over each macro "
				"step: certified by its observed order and error against the exact solution; not a liquid.";
			c.evidence.cases = Evidence(c.effectiveParameters);
		} else {
			caps.earnableFidelity = {"preview"};
			caps.fidelityLimit = "the staggered fluid lags the body's acceleration by a step";
		}
		d = std::move(c);
		configured_ = true;
		return true;
	}
	bool Start(std::string& error) override {
		if (!configured_) return Fail(error, "configure first");
		t_ = 0; next_ = 0; finished_ = false; force_ = 0; havePrevious_ = false; vPrevious_ = 0;
		started_ = true;
		return true;
	}
	bool Restore(const Bytes& state, std::string& error) override {
		if (!configured_) return Fail(error, "configure first");
		V v;
		if (!RISECBOR64::DecodeCanonical(state, v, &error) || v.GetType() != V::Map) return Fail(error, "invalid state");
		const V *t = v.Find("t"), *n = v.Find("next"), *f = v.Find("force"), *h = v.Find("have"), *p = v.Find("vprev");
		if (!t || !n || !f || !h || !p) return Fail(error, "invalid state fields");
		t_ = t->GetFloat(); next_ = static_cast<std::int64_t>(n->GetIntegerArgument()); force_ = f->GetFloat();
		havePrevious_ = h->GetBoolean(); vPrevious_ = p->GetFloat();
		finished_ = static_cast<std::uint64_t>(next_) >= count_;
		started_ = true;
		return true;
	}
	bool Checkpoint(Bytes& state, std::string& error) override {
		return RISECBOR64::Encode(V::MapValue({{"force", V::Float(force_)}, {"have", V::Bool(havePrevious_)},
			{"next", V::Unsigned(static_cast<std::uint64_t>(next_))}, {"t", V::Float(t_)}, {"vprev", V::Float(vPrevious_)}}), state, &error);
	}
	SolverPosition Position() const override { return {t_, next_, finished_}; }
	void RequestStop() override {}

	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress*,
		AdvanceResult& result, std::string& error) override {
		result = AdvanceResult{};
		if (!started_) return Fail(error, "start first");
		const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
		if (macro && request.macroEnd < t_) return Fail(error, "macro end lies in the past");
		bool offered = false;
		for (;;) {
			while (!finished_ && t_ == OutputTime(next_)) {
				std::shared_ptr<const Bytes> load;
				if (!Load(load, error) || !sink.Publish(next_, "load", t_, load, {}, error)) return false;
				if (ports && !ports->Offer("body_load", t_, load, error)) return false;
				offered = true;
				++result.framesPublished;
				if (static_cast<std::uint64_t>(++next_) == count_) finished_ = true;
				if (!macro) return Done(result, AdvanceResult::Status::Advanced);
			}
			if (finished_) return Done(result, AdvanceResult::Status::Finished);
			if (macro && t_ == request.macroEnd) {
				if (!offered && ports) {
					std::shared_ptr<const Bytes> load;
					if (!Load(load, error) || !ports->Offer("body_load", t_, load, error)) return false;
				}
				return Done(result, AdvanceResult::Status::Advanced);
			}
			const double target = macro ? std::min(OutputTime(next_), request.macroEnd) : OutputTime(next_);
			if (!Step(target - t_, ports, error)) return false;
			t_ = target;
			offered = false;
		}
	}

private:
	double OutputTime(std::int64_t i) const { return static_cast<double>(i)*interval_; }
	bool Done(AdvanceResult& r, AdvanceResult::Status s) {
		r.status = finished_ ? AdvanceResult::Status::Finished : s;
		r.position = Position();
		return true;
	}
	bool Step(double h, IPortExchange* ports, std::string& error) {
		if (!ports) { force_ = 0; return true; }
		std::shared_ptr<const Bytes> bytes;
		const PortStatus status = ports->Accept("body_state", t_, bytes, error);
		if (status == PortStatus::Failed) return false;
		if (status != PortStatus::Ready) { force_ = 0; return true; }
		TransformFrame f;
		if (!DecodeTransforms(*bytes, CodecBudget, f, error) || f.bodies.size() != 1) return Fail(error, "one body expected");
		body_ = f.bodies[0].name;
		double y = f.bodies[0].position[1], v = f.bodies[0].velocity[1];
		if (mode_ == "staggered") {
			const double a = havePrevious_ ? (v - vPrevious_)/h : 0.0;
			force_ = -ma_*a - k_*y;
			vPrevious_ = v; havePrevious_ = true;
			return true;
		}
		// Owner: the body's inertia from its mass properties, then the joint step.
		std::shared_ptr<const Bytes> massBytes;
		if (ports->Accept("body_mass_properties", t_, massBytes, error) != PortStatus::Ready)
			return Fail(error, "the owner needs body_mass_properties: " + error);
		FieldFrame mf;
		if (!DecodeField(*massBytes, CodecBudget, mf, error) || mf.count != 1) return false;
		double m = 0;
		for (const auto& ch : mf.channels) if (ch.name == "mass") m = FromF32(ch.values[0]);
		if (!(m > 0)) return Fail(error, "invalid body mass");
		bodyMass_ = m;
		force_ = AddedMassJointStep(m, ma_, k_, g_, h, y, v);
		return true;
	}
	bool Load(std::shared_ptr<const Bytes>& out, std::string& error) const {
		FieldFrame f;
		f.domain = TransformBodyDomain({body_});
		f.count = 1;
		f.channels = {{"force", "N", 3, {ToF32(0), ToF32(force_), ToF32(0)}}, {"point", "m", 3, {ToF32(0), ToF32(0), ToF32(0)}},
			{"torque", "N m", 3, {ToF32(0), ToF32(0), ToF32(0)}}};
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeField(f, CodecBudget, *bytes, error)) return false;
		out = std::move(bytes);
		return true;
	}
	//! Evidence of the joint solve: observed order and error at the configured
	//! step on a canonical problem (m = 1, ma = 4, k = 10, from rest at y = 0.1).
	std::vector<EvidenceCase> Evidence(const V& configuration) const {
		const auto run = [&](double h, double T) {
			double y = 0.1, v = 0;
			const int n = static_cast<int>(std::llround(T/h));
			for (int i = 0; i < n; ++i) AddedMassJointStep(1, 4, 10, g_, h, y, v);
			double ye, ve;
			AddedMassExact(1, 4, 10, g_, 0.1, 0, T, ye, ve);
			return std::fabs(y - ye);
		};
		const double e1 = run(interval_, 1.0), e2 = run(interval_/2, 1.0);
		EvidenceCase order;
		order.id = "joint-solve-order";
		order.description = "observed order of the joint Crank-Nicolson step against the exact oscillator";
		order.error = std::fabs(std::log2(e1/e2) - 2);
		order.tolerance = 0.2;
		order.passed = order.error <= order.tolerance;
		order.digest = EvidenceDigest(order.id, configuration, order.error, order.tolerance, order.passed);
		EvidenceCase accuracy;
		accuracy.id = "joint-solve-error";
		accuracy.description = "displacement error after 1 s relative to the initial amplitude";
		accuracy.error = e1/0.1;
		accuracy.tolerance = 0.05;
		accuracy.passed = accuracy.error <= accuracy.tolerance;
		accuracy.digest = EvidenceDigest(accuracy.id, configuration, accuracy.error, accuracy.tolerance, accuracy.passed);
		return {order, accuracy};
	}
	static std::uint32_t ToF32(double x) { const float f = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &f, 4); return b; }
	static double FromF32(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

	bool configured_ = false, started_ = false, finished_ = false, havePrevious_ = false;
	std::string mode_ = "owner", body_ = "body";
	double ma_ = 1, k_ = 10, g_ = 9.81, interval_ = 0.05, t_ = 0, force_ = 0, vPrevious_ = 0, bodyMass_ = 0;
	std::uint64_t count_ = 41;
	std::int64_t next_ = 0;
};

inline void RegisterAddedMassSolver()
{
	std::string ignored;
	RegisterSolver(AddedMassSolver::Id, []{ return std::unique_ptr<ISimulationSolver>(new AddedMassSolver); }, ignored);
}

} } }
#endif
