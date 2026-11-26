// A coupled-oscillator test solver for the M6 group host (test code only).
//
//   test.oscillator   one mass on a wall spring, coupled by a spring to a partner
//                     whose displacement it accepts through the port "partner";
//                     m u'' = -k u + kc (u_partner(t) - u).  Offers "state"
//                     (field.v1, one element: the displacement [units]).
//
// The accepted partner value is the partner's displacement at the END of the
// macro step; over the step the partner moves linearly from the previously
// held value (a Jacobi exchange therefore lags the partner by one step, and an
// implicit exchange converges to a second-order coupled scheme).  Velocity
// Verlet substeps.  Honours macro steps, rollback, checkpoints; deterministic.
// Two of them reproduce the two-mass, three-spring system whose exact
// solution TwoOscillatorExact gives.
#ifndef RISE_TESTS_SIMULATION_COUPLING_SOLVERS_H
#define RISE_TESTS_SIMULATION_COUPLING_SOLVERS_H

#include "../src/Library/Simulation/SimulationSolver.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "SimulationReferenceSolvers.h"

namespace RISE { namespace Simulation { namespace Reference {

inline std::string OscillatorDomain()
{
	return RISECBOR64::SHA256Hex(Bytes{'o', 's', 'c'});
}

class OscillatorSolver final : public ISimulationSolver {
public:
	static constexpr const char* Id = "test.oscillator";
	bool Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error) override {
		configured_ = started_ = false;
		Params p(parameters, error);
		mass_ = p.Real("mass", 1);
		k_ = p.Real("stiffness", 4);
		kc_ = p.Real("coupling", 1);
		u0_ = p.Real("displacement", 0);
		v0_ = p.Real("velocity", 0);
		count_ = p.Count("frames", 41, 1, 1000000);
		interval_ = p.Real("interval", 0.05);
		substeps_ = p.Count("substeps", 20, 1, 100000);
		units_ = p.Text("units", "m");
		const std::string fidelity = p.Text("fidelity", "predictive");
		SolverDescription c;
		if (!p.Finish(c.effectiveParameters)) return false;
		if (!(mass_ > 0) || !(interval_ > 0) || (units_ != "m" && units_ != "cm")) return Fail(error, "invalid oscillator parameters");
		scale_ = units_ == "cm" ? 100 : 1;
		c.solverId = Id; c.version = "1";
		c.cache.count = count_; c.cache.interval = interval_;
		c.cache.outputs = {{"state", "field.v1", {{"displacement", units_, "f32"}}}};
		c.ports = {
			{"state", "oscillator state", units_, "field.v1", true, "state",
				{{"displacement", "displacement", units_, 1}}, OscillatorDomain()},
			{"partner", "oscillator state", units_, "field.v1", false, "",
				{{"displacement", "displacement", units_, 1}}, OscillatorDomain()}};
		auto& caps = c.capabilities;
		caps.backends = {{"cpu", BackendStatus::Supported, true, "test"}};
		caps.interactive = caps.deterministic = caps.resumable = caps.externalMacroStep = caps.rollback = true;
		caps.threadCountInvariant = true;
		caps.earnableFidelity = {"preview", "predictive"};
		c.evidence.fidelity = fidelity;
		EvidenceCase e;
		e.id = "verlet"; e.error = 0; e.tolerance = 1; e.passed = true;
		e.digest = EvidenceDigest(e.id, c.effectiveParameters, 0, 1, true);
		c.evidence.cases = {e};
		c.evidence.conditions = {{"integrator", {"verlet"}}};
		c.evidence.scope = "test oscillator";
		configured_ = true;
		d = std::move(c);
		return true;
	}
	bool Start(std::string& error) override {
		if (!configured_) return Fail(error, "configure first");
		s_ = State();
		s_.u = u0_; s_.v = v0_;
		s_.finished = count_ == 0;
		started_ = true;
		return true;
	}
	SolverPosition Position() const override { return {s_.t, static_cast<std::int64_t>(s_.next), s_.finished}; }
	void RequestStop() override { stop_ = true; }
	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress*, AdvanceResult& result,
		std::string& error) override {
		result = AdvanceResult{};
		if (!started_) return Fail(error, "start first");
		const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
		if (macro && request.macroEnd < s_.t) return Fail(error, "macro end lies in the past");
		if (!macro) s_.held = false;
		bool offered = false;
		for (;;) {
			while (!s_.finished && s_.t == Out(s_.next)) {
				auto payload = Encode();
				if (!sink.Publish(static_cast<std::int64_t>(s_.next), "state", s_.t, payload, {}, error)) return false;
				if (ports && !ports->Offer("state", s_.t, payload, error)) return false;
				offered = true;
				++result.framesPublished;
				if (++s_.next == count_) s_.finished = true;
				if (!macro) return Done(result, AdvanceResult::Status::Advanced);
			}
			if (s_.finished) return Done(result, AdvanceResult::Status::Finished);
			if (macro && s_.t == request.macroEnd && s_.segN == 0) {
				if (!offered && ports && !ports->Offer("state", s_.t, Encode(), error)) return false;
				return Done(result, AdvanceResult::Status::Advanced);
			}
			if (s_.segN == 0) {
				s_.segStart = s_.t;
				s_.segTarget = macro ? std::min(Out(s_.next), request.macroEnd) : Out(s_.next);
				const double nominal = interval_/static_cast<double>(substeps_);
				s_.segN = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((s_.segTarget - s_.segStart)/nominal*(1 - 1e-12))));
				s_.segJ = 0;
				if (!macro || !s_.held || s_.heldFor != request.macroEnd) {
					if (!AcceptPartner(ports, error)) return false;
					s_.held = macro;
					s_.heldFor = request.macroEnd;
					s_.holdStart = s_.segStart;
					s_.holdEnd = macro ? request.macroEnd : s_.segTarget;
				}
			} else if (macro && s_.segTarget > request.macroEnd) return Fail(error, "macro end inside an interrupted segment");
			offered = false;
			while (s_.segJ < s_.segN) {
				if (stop_.load()) { stop_ = false; return Done(result, AdvanceResult::Status::Stopped); }
				const double tn = s_.segJ + 1 == s_.segN ? s_.segTarget :
					s_.segStart + (s_.segTarget - s_.segStart)*static_cast<double>(s_.segJ + 1)/static_cast<double>(s_.segN);
				const double h = tn - s_.t;
				s_.v += 0.5*h*Acceleration(s_.u, s_.t);
				s_.u += h*s_.v;
				s_.v += 0.5*h*Acceleration(s_.u, tn);
				s_.t = tn;
				++s_.segJ;
			}
			s_.segN = s_.segJ = 0;
		}
	}
	bool Checkpoint(Bytes& state, std::string& error) override {
		if (!started_) return Fail(error, "nothing to checkpoint");
		const std::vector<double> d = {s_.t, s_.segStart, s_.segTarget, s_.heldFor, s_.u, s_.v, s_.partnerStart, s_.partnerEnd,
			s_.holdStart, s_.holdEnd};
		return RISECBOR64::Encode(V::MapValue({{"format", V::String("test.oscillator.v1")}, {"d", V::BytesValue(PackDoubles(d))},
			{"next", V::Unsigned(s_.next)}, {"segN", V::Unsigned(s_.segN)}, {"segJ", V::Unsigned(s_.segJ)},
			{"flags", V::Unsigned((s_.finished ? 1u : 0u) | (s_.held ? 2u : 0u) | (s_.havePartner ? 4u : 0u))}}), state, &error);
	}
	bool Restore(const Bytes& state, std::string& error) override {
		if (!configured_) return Fail(error, "configure first");
		V v;
		std::vector<double> d;
		if (!RISECBOR64::DecodeCanonical(state, v, &error) || !UnpackDoubles(v.Find("d"), 10, d)) return Fail(error, "invalid oscillator state");
		s_ = State();
		s_.t = d[0]; s_.segStart = d[1]; s_.segTarget = d[2]; s_.heldFor = d[3]; s_.u = d[4]; s_.v = d[5];
		s_.partnerStart = d[6]; s_.partnerEnd = d[7]; s_.holdStart = d[8]; s_.holdEnd = d[9];
		s_.next = v.Find("next")->GetIntegerArgument(); s_.segN = v.Find("segN")->GetIntegerArgument(); s_.segJ = v.Find("segJ")->GetIntegerArgument();
		const auto f = v.Find("flags")->GetIntegerArgument();
		s_.finished = f & 1; s_.held = (f & 2) != 0; s_.havePartner = (f & 4) != 0;
		started_ = true;
		return true;
	}
	bool SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error) override {
		if (!started_) return Fail(error, "nothing to save");
		auto t = std::make_shared<TokenWith>();
		t->owner = this; t->state = s_;
		token = t;
		return true;
	}
	bool RestoreState(const SolverStateToken& token, std::string& error) override {
		const auto* t = dynamic_cast<const TokenWith*>(&token);
		if (!t || t->owner != this) return Fail(error, "foreign token");
		s_ = t->state;
		return true;
	}
	double Displacement() const { return s_.u; }
private:
	struct State {
		double t = 0, segStart = 0, segTarget = 0, heldFor = 0, u = 0, v = 0, partnerStart = 0, partnerEnd = 0, holdStart = 0, holdEnd = 0;
		std::uint64_t next = 0, segN = 0, segJ = 0;
		bool finished = false, held = false, havePartner = false;
	};
	struct TokenWith final : SolverStateToken { const void* owner = nullptr; State state; };
	double Out(std::uint64_t i) const { return static_cast<double>(i)*interval_; }
	bool Done(AdvanceResult& r, AdvanceResult::Status st) {
		r.status = s_.finished ? AdvanceResult::Status::Finished : st;
		r.position = Position();
		return true;
	}
	double Partner(double t) const {
		if (!s_.havePartner) return 0;
		if (!(s_.holdEnd > s_.holdStart)) return s_.partnerEnd;
		const double f = (t - s_.holdStart)/(s_.holdEnd - s_.holdStart);
		return s_.partnerStart + f*(s_.partnerEnd - s_.partnerStart);
	}
	double Acceleration(double u, double t) const {
		const double coupled = s_.havePartner ? kc_*(Partner(t) - u) : 0.0;
		return (-k_*u + coupled)/mass_;
	}
	std::shared_ptr<const Bytes> Encode() const {
		FieldFrame f;
		f.domain = OscillatorDomain(); f.count = 1;
		f.channels = {{"displacement", units_, 1, {F32(s_.u*scale_)}}};
		auto bytes = std::make_shared<Bytes>();
		std::string error;
		EncodeField(f, CodecBudget, *bytes, error);
		return bytes;
	}
	bool AcceptPartner(IPortExchange* ports, std::string& error) {
		if (!ports) return true;
		std::shared_ptr<const Bytes> bytes;
		const auto status = ports->Accept("partner", s_.segStart, bytes, error);
		if (status == PortStatus::Unbound || status == PortStatus::Unavailable) return true;
		if (status != PortStatus::Ready) return false;
		FieldFrame f;
		if (!DecodeField(*bytes, CodecBudget, f, error) || f.channels.empty() || f.channels[0].name != "displacement" ||
			f.channels[0].units != units_) return Fail(error, "partner must be an oscillator state in " + units_);
		const double u = FromF32(f.channels[0].values[0])/scale_;
		s_.partnerStart = s_.havePartner ? s_.partnerEnd : u;
		s_.partnerEnd = u;
		s_.havePartner = true;
		return true;
	}
	bool configured_ = false, started_ = false;
	double mass_ = 1, k_ = 4, kc_ = 1, u0_ = 0, v0_ = 0, interval_ = 0.05, scale_ = 1;
	std::uint64_t count_ = 41, substeps_ = 20;
	std::string units_ = "m";
	State s_;
	std::atomic<bool> stop_{false};
};

//! Exact displacements of the two-mass, three-spring system at time t.
inline void TwoOscillatorExact(double ma, double ka, double mb, double kb, double kc, double ua, double va, double ub, double vb,
	double t, double& outA, double& outB)
{
	// A = M^-1/2 K M^-1/2, symmetric 2x2.
	const double sa = 1/std::sqrt(ma), sb = 1/std::sqrt(mb);
	const double a11 = (ka + kc)*sa*sa, a22 = (kb + kc)*sb*sb, a12 = -kc*sa*sb;
	const double tr = a11 + a22, det = a11*a22 - a12*a12, disc = std::sqrt(tr*tr/4 - det);
	const double l1 = tr/2 - disc, l2 = tr/2 + disc;
	// Eigenvectors.
	double e1[2] = {a12, l1 - a11}, e2[2] = {a12, l2 - a11};
	if (std::fabs(a12) < 1e-300) { e1[0] = 1; e1[1] = 0; e2[0] = 0; e2[1] = 1; }
	const double n1 = std::hypot(e1[0], e1[1]), n2 = std::hypot(e2[0], e2[1]);
	e1[0] /= n1; e1[1] /= n1; e2[0] /= n2; e2[1] /= n2;
	// Mass-scaled coordinates y = M^1/2 u.
	const double y0[2] = {ua/sa, ub/sb}, w0[2] = {va/sa, vb/sb};
	const double q1 = e1[0]*y0[0] + e1[1]*y0[1], q2 = e2[0]*y0[0] + e2[1]*y0[1];
	const double p1 = e1[0]*w0[0] + e1[1]*w0[1], p2 = e2[0]*w0[0] + e2[1]*w0[1];
	const double w1 = std::sqrt(l1), w2 = std::sqrt(l2);
	const double r1 = q1*std::cos(w1*t) + p1/w1*std::sin(w1*t), r2 = q2*std::cos(w2*t) + p2/w2*std::sin(w2*t);
	outA = sa*(r1*e1[0] + r2*e2[0]);
	outB = sb*(r1*e1[1] + r2*e2[1]);
}

inline void RegisterCouplingTestSolvers()
{
	std::string ignored;
	RegisterSolver(OscillatorSolver::Id, []{ return std::unique_ptr<ISimulationSolver>(new OscillatorSolver); }, ignored);
}

} } }
#endif
