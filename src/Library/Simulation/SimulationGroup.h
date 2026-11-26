// Simulation M6: the co-stepping group host (the contract's
// ISimulationGroupHost, SimulationHost.h; docs/SIMULATION_M6_RESULTS.md).
//
// Owns live member solvers, cached members (an immutable snapshot standing in
// for a live upstream, read through CachedPortFeed) and PortConnections.  All
// live members advance by the same macro step (GroupPolicy::macroStep, or the
// smallest output interval of the live members) and exchange port values at
// its boundaries:
//   * explicit staggered: one pass per step.  Jacobi: every member reads the
//     values offered at the step start.  Gauss-Seidel: members advance in
//     memberOrder and a later member reads an earlier member's end-of-step
//     offer.
//   * implicit (subIterations > 1, every live member with rollback): the
//     members are saved, advanced, and while the exchanged values change by
//     more than the tolerance they are restored and advanced again with the
//     previous iterate's end-of-step values, under-relaxed (Aitken's dynamic
//     factor when asked).  Frames of a repeated step are held back until the
//     step is accepted.  Without convergence after subIterations the step
//     fails with a diagnostic, or (NonConvergence::Halve) is retried as two
//     half steps down to minimumMacroStep.
// Deterministic.  Checkpoint/Restore cover the whole group at a macro
// boundary (member checkpoints plus the exchanged values in flight).  Every
// member publishes to its own cache (memory, optionally recorded to disk),
// and a recorded member gets the group manifest rise-sim.group.cbor tying the
// epochs together.
//
// Keys: a member that reads through a connection (directly or from upstream
// of it) gets one more key input, kCouplingInput, whose identity covers the
// policy, every member in its upstream closure (solver, version, effective
// parameters and input identities; a cached member's key, plus its manifest
// digest when foreign) and every connection into it with its adapter's
// identity.  A member nothing feeds keeps its standalone key.
//
// Labels: a coupled member is predictive only when it is itself, every member
// in its closure is, every adapter on those connections is exact
// (IPortAdapter::Fidelity), the policy is implicit with a tolerance of at most
// kPredictiveCouplingTolerance, and the coupling kind has registered evidence
// (RegisterCouplingEvidence) whose cases pass; the coupling cases then join
// the member's evidence with the condition "coupling".  Otherwise preview.
#ifndef RISE_SIMULATION_GROUP_H
#define RISE_SIMULATION_GROUP_H

#include "SimulationHost.h"
#include <atomic>
#include <map>

namespace RISE { namespace Simulation {

//! The key input a coupled member's description gains.
extern const char* const kCouplingInput;          // "rise.coupling"
//! The group manifest a recorded member's cache directory holds.
extern const char* const kGroupManifestName;      // "rise-sim.group.cbor"
//! Implicit tolerance a predictive coupling may not exceed.
constexpr double kPredictiveCouplingTolerance = 1e-6;

//! Coupling evidence for a kind: the sorted, unique adapter ids of the
//! connections, joined by "+" ("direct" for a connection without one).
//! `compute` fills cases, conditions and scope (fidelity "predictive" when
//! it may be earned); it runs once per process, memoised.
bool RegisterCouplingEvidence(const std::string& kind, std::function<bool(Evidence&, std::string&)> compute,
	std::string& error);
bool CouplingEvidence(const std::string& kind, Evidence& evidence, std::string& error);

class SimulationGroup final : public ISimulationGroupHost {
public:
	SimulationGroup();
	~SimulationGroup() override;
	SimulationGroup(const SimulationGroup&) = delete;
	SimulationGroup& operator=(const SimulationGroup&) = delete;

	// ISimulationGroupHost.  Members and connections before Configure.
	bool AddMember(const std::string& name, std::unique_ptr<ISimulationSolver> solver,
		const RISECBOR64::Value& parameters, std::string& error) override;
	bool AddCachedMember(const std::string& name, std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot,
		std::string& error) override;
	bool Connect(const PortConnection& connection, std::string& error) override;
	//! `inputs` serves every live member without its own (SetMemberInputs) and must outlive the group.
	bool Configure(const GroupPolicy& policy, const ISceneInputs& inputs, std::string& error) override;
	bool Start(std::string& error) override;
	bool StepMacro(std::string& error) override;
	void RequestStop() override { stop_ = true; }
	bool Checkpoint(Bytes& state, std::string& error) override;
	//! After Configure, instead of Start.
	bool Restore(const Bytes& state, std::string& error) override;
	std::shared_ptr<const IReadOnlyCacheSnapshot> Snapshot(const std::string& member) const override;

	// M6 extras.
	//! A foreign cached member (simulation_cache): its manifest digest joins the identity.
	bool AddCachedMember(const std::string& name, std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot, bool foreign,
		std::string& error);
	//! A cached member read at cache time = group time + offset (before Configure; enters the identity).
	bool SetCachedTimeOffset(const std::string& name, double offset, std::string& error);
	//! The scene inputs of one live member (before Configure; must outlive the group).
	bool SetMemberInputs(const std::string& name, const ISceneInputs& inputs, std::string& error);
	//! After Configure, before Start or Restore: also write the member's frames to a disk cache
	//! (a new epoch, or `resume` a partial one of the same key and epoch).
	bool Record(const std::string& member, const std::filesystem::path& root, std::size_t budget, bool resume,
		std::string& error);
	//! Seals every recording (when the run has finished).
	bool SealRecordings(std::string& error);
	//! Steps until every live member has finished (or a step fails or a stop is requested).
	bool Run(std::string& error);

	const SolverDescription* Description(const std::string& member) const;   // live members, after Configure
	std::vector<std::string> Members() const;
	bool IsCached(const std::string& member) const;
	double MacroStep() const { return macroStep_; }
	double Time() const { return time_; }
	double EndTime() const { return end_; }
	std::uint64_t Steps() const { return steps_; }
	bool Finished() const;
	bool StopRequested() const { return stop_; }
	//! SHA-256 over the policy, every member and every connection (the group manifest's id).
	const std::string& GroupIdentity() const { return identity_; }
	//! The fidelity a member's cache carries (after Configure).
	std::string MemberFidelity(const std::string& member) const;
	//! Why the coupling of a member is not predictive (empty when it is, or when nothing feeds it).
	std::string CouplingLimit(const std::string& member) const;

	struct StepReport {
		unsigned iterations = 0;        // passes of the last accepted (sub)step
		unsigned totalIterations = 0;   // over the whole macro step, halvings included
		unsigned halvings = 0;
		double residual = 0;            // largest relative residual of the accepted iterate
		std::vector<std::pair<std::string,double>> connectionResiduals;
	};
	const StepReport& LastStep() const { return report_; }
	//! A diagnostic hook: called after every accepted (sub)step with its end time.
	void SetStepObserver(std::function<void(double, const SimulationGroup&)> observer) { observer_ = std::move(observer); }
	//! The value a connection served in the last accepted iterate (decoded by the caller), for diagnostics.
	std::shared_ptr<const Bytes> ServedValue(std::size_t connection) const;
	std::size_t ConnectionCount() const;

private:
	struct Member;
	struct Link;
	class Exchange;
	class Sink;
	bool StepInterval(double a, double b, int depth, std::string& error);
	bool AdvanceAll(double b, bool buffered, std::string& error);
	bool Serve(Link& link, std::shared_ptr<const Bytes>& value, std::string& error);
	bool Latest(const Member& m, const std::string& port, double limit, std::shared_ptr<const Bytes>& value) const;
	PortStatus ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
		std::string& error) const;
	void Prune(double t);
	void DropAfter(double t);
	bool WriteGroupManifests(std::string& error);

	std::vector<std::unique_ptr<Member>> members_;
	std::vector<std::unique_ptr<Link>> links_;
	std::vector<std::size_t> order_;            // live members in stepping order
	GroupPolicy policy_;
	const ISceneInputs* inputs_ = nullptr;
	bool configured_ = false, started_ = false;
	double macroStep_ = 0, time_ = 0, end_ = 0;
	std::uint64_t steps_ = 0;
	std::string identity_;
	// The pass in progress.
	double stepStart_ = 0, stepEnd_ = 0;
	unsigned iteration_ = 0;
	StepReport report_;
	std::function<void(double, const SimulationGroup&)> observer_;
	std::atomic<bool> stop_{false};
};

} }
#endif
