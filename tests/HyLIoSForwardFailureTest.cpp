// Compile separately with this deterministic fault provider INSTEAD OF the
// strict-FP overlap implementation. It verifies transport's consumer contract;
// actual geometry has independent certified-predicate tests.
#include "../src/Library/Materials/HyLIoS/HyLIoSParticleOverlap.h"
namespace RISE::HyLIoS {
ParticleOverlapStatus injectedStatus=ParticleOverlapStatus::Indeterminate;
ParticleOverlapResult ClassifyProlateOverlap(const ProlateParticle&,const ProlateParticle&,ParticleVector) {return {injectedStatus,1};}
}
#define main transport_test_main
#include "HyLIoSForwardTransportTest.cpp"
#undef main
int main(){try {
 for(auto policy:{ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPlacementPolicy::NullCollisionPlacementV1})
 for(auto status:{ParticleOverlapStatus::Indeterminate,ParticleOverlapStatus::InvalidInput,ParticleOverlapStatus::UnsupportedRange}) {
  RISE::HyLIoS::injectedStatus=status;
  HyLIoSForwardTransport k(Particles(),1400,1,0,AbsorptionEstimator::Analog,0,policy);auto s=Enter(k);
  Sequence t{.3};k.Advance(s,t,1);Sequence p{.5,0,0,0};k.Advance(s,p,1);Sequence e{.5};k.Advance(s,e,1);Sequence delta{};k.Advance(s,delta,1);
  Sequence next{.01};k.Advance(s,next,1);Sequence attempt{.5,0,0,0};k.Advance(s,attempt,1);
  Check(s.Termination()==ForwardTermination::NumericalFailure&&s.Weight()==1&&Absorbed(s)==0,"uncertain overlap is unresolved live power, never a reject/null/absorb");
  Check(s.Counters().acceptedParticles==1&&s.Counters().overlapRejections==0&&s.Counters().nullPlacements==0,"overlap failure preserves accepted predecessor and rejection counters");
  const auto expected=status==ParticleOverlapStatus::Indeterminate?ForwardFailureDetail::OverlapIndeterminate:status==ParticleOverlapStatus::InvalidInput?ForwardFailureDetail::OverlapInvalidInput:ForwardFailureDetail::OverlapUnsupportedRange;
  Check(s.FailureDetail()==expected,"overlap numerical diagnostic retains predicate failure kind");
 }
 std::cout<<"PASS "<<checks<<" injected overlap failure checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
