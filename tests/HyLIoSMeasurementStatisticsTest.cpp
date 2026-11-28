// Exercise the production CLI's private accumulators without a public test hook.
#define main hylios_measure_program_main
#include "../tools/hylios_measure.cpp"
#undef main
#include <functional>
namespace {
int checks=0;
void Check(bool condition,const char* message) {
    ++checks;if(!condition) throw std::runtime_error(message);
}
void Close(double actual,double expected,const char* message) {
    const double bound=2048*std::numeric_limits<double>::epsilon()*std::abs(expected);
    Check(std::isfinite(actual)&&std::abs(actual-expected)<=bound,message);
}
template<class F> void Reject(F operation,const char* message) {
    bool threw=false;try {operation();}catch(const std::exception&) {threw=true;}Check(threw,message);
}
// Independent dense Welford consumes every observation, including interleaved
// zeros. The production accumulator observes nonzeros in a moving scale and
// merges the missing zero population only when summarizing.
struct DenseReference {
    std::uint64_t n=0;double mean=0,m2=0;
    void Add(double x) {++n;const double delta=x-mean;mean+=delta/static_cast<double>(n);m2+=delta*(x-mean);}
    double SE() const {return std::sqrt(m2/static_cast<double>(n)/static_cast<double>(n-1));}
};
void SparseDense() {
    SparseMoments sparse,splitA,splitB;DenseReference dense;
    std::uint64_t nz=0,bits=0x48394c496f53ULL;
    for(std::uint64_t i=0;i<20000;++i) {
        bits=bits*6364136223846793005ULL+1442695040888963407ULL;
        const double x=i%7==0 ? 0:static_cast<double>((bits>>32)%10000)/1000;
        dense.Add(x);sparse.Observe(x);(i%2==0 ? splitA:splitB).Observe(x);nz+=x>0;
    }
    const auto result=sparse.Summary(dense.n);splitA.MergeNonzero(splitB);const auto merged=splitA.Summary(dense.n);
    Check(result.nonzeroCount==nz,"sparse count excludes zero histories");
    Close(result.mean,dense.mean,"sparse/dense all-history mean agreement");
    Close(*result.standardError,dense.SE(),"sparse/dense all-history SE agreement");
    Close(merged.mean,dense.mean,"merged disjoint nonzero streams retain dense mean");
    Close(*merged.standardError,dense.SE(),"merged disjoint streams retain dense SE");
    Close(result.sumWeight/dense.n,dense.mean,"sum weight uses the same incident denominator");
    SparseMoments empty;const auto none=empty.Summary(1000000000000000000ULL);
    Check(none.mean==0&&none.sumWeight==0&&*none.standardError==0,"huge all-zero population is exact");
    const auto oneZero=empty.Summary(1);Check(!oneZero.standardError,"single zero observation has null SE");
    SparseMoments one;one.Observe(3);const auto singleton=one.Summary(1);
    Check(singleton.mean==3&&singleton.sumWeight==3&&!singleton.standardError,"single nonzero observation has null SE");
    one.Observe(0);Check(one.Count()==1,"observed zeros stay implicit");
    SparseMoments few;few.Observe(1);few.Observe(3);constexpr std::uint64_t huge=1000000000000000000ULL;
    const auto rare=few.Summary(huge);const double n=static_cast<double>(huge);
    Close(rare.mean,4/n,"huge denominator mean agrees with independent sum identity");
    Close(*rare.standardError,std::sqrt((10-16/n)/n/(n-1)),"huge denominator SE agrees with independent exact second-moment identity");
    Reject([&]{few.Summary(1);},"denominator below nonzero population is rejected");
    Reject([&]{few.Summary(0);},"zero denominator is rejected");
    Reject([&]{few.Observe(-1);},"negative contribution is rejected");
    Reject([&]{few.Observe(std::numeric_limits<double>::infinity());},"nonfinite contribution is rejected");
    Check(few.Count()==2,"rejected observations preserve count");
}
void ScaleAndRange() {
    Moments scalar;for(double x:{1e-300,3e-300,0.,0.}) scalar.Add(x);
    const auto scalarJson=scalar.Json();
    Close(scalarJson.get("mean").asNumber(),1e-300,"scalar statistics share stable tiny mean");
    Close(scalarJson.get("standard_error").asNumber(),1e-300*std::sqrt(.5),"scaled_scalar_standard_error preserves representable tiny uncertainty");
    for(double scale:{1e-300,1.,1e150}) {
        SparseMoments values;values.Observe(scale);values.Observe(3*scale);
        const auto result=values.Summary(4);
        Close(result.mean,scale,"scaled sparse mean follows exact unit identity");
        Close(*result.standardError,scale*std::sqrt(.5),"scaled sparse SE survives tiny/large squared weights");
    }
    SparseMoments extreme;extreme.Observe(1e308);extreme.Observe(1);
    const auto wide=extreme.Summary(2);Close(wide.mean,5e307,"large nonnegative mean does not square physical weights");
    Close(*wide.standardError,5e307,"representable SE survives nonrepresentable unscaled variance");
    SparseMoments overflow;overflow.Observe(std::numeric_limits<double>::max());overflow.Observe(std::numeric_limits<double>::max());
    Reject([&]{overflow.Summary(2);},"unrepresentable published sum is an explicit diagnostic");
    Check(overflow.Count()==2,"publication range failure preserves observations");
    SparseMoments underflow;underflow.Observe(std::numeric_limits<double>::denorm_min());
    Reject([&]{underflow.Summary(3);},"unrepresentable positive published mean is not silently zero");
    SparseMoments count;count.Observe(1);
    for(int bit=0;bit<63;++bit) {const auto copy=count;count.MergeNonzero(copy);}
    Check(count.Count()==(std::uint64_t{1}<<63),"large counts merge without a giant update loop");
    const auto copy=count;Reject([&]{count.MergeNonzero(copy);},"count overflow is rejected before mutation");
    Check(count.Count()==copy.Count(),"count-overflow failure preserves state");
}
void AdjacentVariance() {
    for(int exponent:{-900,-484,-200,0,200,500,900}) {
        const double a=std::ldexp(0x1.0000000000001p0,exponent);
        const double b=std::ldexp(0x1.0000000000002p0,exponent);
        const double step=std::ldexp(1.,exponent-52);
        for(const auto& observations:std::vector<std::vector<double>>{{a,b},{b,a},{a,b,a},{b,a,b},{a,b,a,b},{b,a,b,a}}) {
            SparseMoments moments;Moments scalar;
            for(double value:observations) {moments.Observe(value);scalar.Add(value);}
            const double n=static_cast<double>(observations.size());
            const double na=static_cast<double>(std::count(observations.begin(),observations.end(),a));
            const double nb=n-na;
            // Exact two-level population identity, no rounded sample mean or
            // squared physical step in the independent oracle.
            const double expected=step*std::sqrt((na/n)*(nb/n)/(n-1));
            const auto result=moments.Summary(observations.size());
            Check(*result.standardError>0,"adjacent_scaled_variance cannot be falsely zero");
            Close(*result.standardError,expected,"adjacent_scaled_variance agrees with exact pair/triple/quartet rational identity");
            Close(scalar.Json().get("standard_error").asNumber(),expected,"adjacent scalar channels share exact variance identity");
        }
        SparseMoments left,right;
        for(double value:{a,b,a}) left.Observe(value);
        for(double value:{b,a,b}) right.Observe(value);
        left.MergeNonzero(right);
        Close(*left.Summary(6).standardError,step/std::sqrt(20.),"adjacent merged groups preserve centered-mean residual");
    }
}
SparseMoments RepeatedMoments(double value,std::uint64_t count) {
    SparseMoments result,block;block.Observe(value);
    while(count) {
        if(count&1) result.MergeNonzero(block);
        count>>=1;
        if(count) {const auto copy=block;block.MergeNonzero(copy);}
    }
    return result;
}
void CloseAnalytic(double actual,double expected,const char* message) {
    const double bound=64*std::numeric_limits<double>::epsilon()*std::abs(expected);
    Check(std::isfinite(actual)&&std::abs(actual-expected)<=bound,message);
}
void AsymmetricCounts() {
    constexpr double high=1,low=1e-20;
    for(std::uint64_t n:{1000001ULL,10000000ULL,1000000000ULL,1000000000000000000ULL}) {
        SparseMoments one;one.Observe(high);const auto tiny=RepeatedMoments(low,n-1);
        one.MergeNonzero(tiny);const auto result=one.Summary(n);
        const double expectedMean=low+(high-low)/static_cast<double>(n);
        const double expectedSE=(high-low)/static_cast<double>(n);
        CloseAnalytic(result.mean,expectedMean,"asymmetric_mean own nonnegative group fractions preserve rare large mass");
        CloseAnalytic(*result.standardError,expectedSE,"asymmetric groups preserve exact two-level SE");
        auto opposite=tiny;SparseMoments head;head.Observe(high);opposite.MergeNonzero(head);
        CloseAnalytic(opposite.Summary(n).mean,expectedMean,"asymmetric mean is stable under opposite group order");
        CloseAnalytic(*opposite.Summary(n).standardError,expectedSE,"asymmetric SE is stable under opposite group order");
    }
    constexpr std::uint64_t start=1000000000ULL,extra=100000ULL;
    SparseMoments sequential;sequential.Observe(high);sequential.MergeNonzero(RepeatedMoments(low,start-1));
    auto batched=sequential;batched.MergeNonzero(RepeatedMoments(low,extra));
    for(std::uint64_t i=0;i<extra;++i) sequential.Observe(low);
    const double n=static_cast<double>(start+extra),expectedMean=low+(high-low)/n,expectedSE=(high-low)/n;
    CloseAnalytic(sequential.Summary(start+extra).mean,expectedMean,"long_repeat_mean compensated updates do not stall near range minimum");
    CloseAnalytic(*sequential.Summary(start+extra).standardError,expectedSE,"long_repeat_variance compensated increments do not stall below M2 high-part ulp");
    CloseAnalytic(sequential.Summary(start+extra).mean,batched.Summary(start+extra).mean,"long repeat and batch grouping preserve the same mean");
    CloseAnalytic(*sequential.Summary(start+extra).standardError,*batched.Summary(start+extra).standardError,"long repeat and batch grouping preserve the same SE");
}
void CellsAndOverflow() {
    const auto phi=ExitPhiEdges(),mu=ExitMuEdges();
    for(std::size_t i=0;i<ExitAzimuthBins;++i) {
        Check(FiniteCell(phi[i],phi)==i,"azimuth edge belongs to cell starting at that exact recorded edge");
        if(i>0) Check(FiniteCell(std::nextafter(phi[i],0.),phi)==i-1,"azimuth immediately below edge stays in previous cell");
    }
    for(std::size_t i=0;i<ExitCosineBins;++i) Check(FiniteCell(mu[i],mu,true)==i,"cosine edge follows exact half-open rule");
    Check(FiniteCell(1.,mu,true)==ExitCosineBins-1,"cosine one belongs to final cell");
    for(std::size_t i=0;i<ExitRadialEdges.size();++i) {
        const auto at=LocateExit({ExitRadialEdges[i],0,0},{0,0,-1});
        Check(at.radial==i,"radial edge starts next interval, including explicit overflow");
        if(i>0) Check(LocateExit({std::nextafter(ExitRadialEdges[i],0.),0,0},{0,0,-1}).radial==i-1,"radius immediately below edge stays finite previous cell");
    }
    Check(LocateExit({0,0,0},{0,0,-1}).azimuth==0,"exact outgoing pole uses canonical azimuth zero");
    Check(LocateExit({0,0,0},{1,-std::numeric_limits<double>::denorm_min(),0}).azimuth==ExitAzimuthBins-1,"negative seam approach remains final azimuth bin even when wrap rounds to 2pi");
    const double maximum=std::numeric_limits<double>::max();
    const auto outside=LocateExit({maximum,maximum,0},{0,0,-1});
    Check(outside.radial==ExitRadialBins-1&&outside.radiusUnrepresentable,"finite coordinates with unrepresentable radius preserve overflow category");
    Reject([&]{LocateExit({0,0,0},{0,0,1});},"inward direction is not silently clamped into an exit histogram");
    Reject([&]{LocateExit({0,0,0},{0,0,0});},"zero exit direction is rejected");
    ExitHistogram histogram;histogram.Observe({maximum,maximum,0},{0,0,-1},2,true);
    histogram.Observe({0,0,0},{1,0,-1},.5,false);
    const auto report=histogram.Json(5);double all=0,external=0,subsurface=0;
    const auto& cells=report.get("joint_cells");
    for(std::size_t i=0;i<cells.size();++i) {
        all+=cells.at(i).get("all_returns").get("mean").asNumber();
        external+=cells.at(i).get("external_reflection").get("mean").asNumber();
        subsurface+=cells.at(i).get("subsurface_return").get("mean").asNumber();
    }
    Check(report.get("nonzero_cell_updates").asNumber()==4,"joint update counter is two per positive exit, independent of zero histories and bin count");
    Close(all,.5,"overflow and finite bins retain all weighted returned power over all histories");
    Close(external,.4,"external histogram has all-history denominator");Close(subsurface,.1,"subsurface histogram has all-history denominator");
    Check(report.get("unrepresentable_radius_history_count").asNumber()==1,"radius representation overflow remains observable");
    Check(!report.get("is_density").asBool()&&!report.get("production_bake_available").asBool(),"bin masses do not claim density or bake support");
    double angular=0,radial=0;
    for(std::size_t i=0;i<report.get("angular_marginal").size();++i) angular+=report.get("angular_marginal").at(i).get("all_returns").get("mean").asNumber();
    for(std::size_t i=0;i<report.get("radial_marginal").size();++i) radial+=report.get("radial_marginal").at(i).get("all_returns").get("mean").asNumber();
    Close(angular,all,"angular marginal derives the same joint mass");Close(radial,all,"radial marginal derives the same joint mass");
}
}
int main() {try {SparseDense();ScaleAndRange();AdjacentVariance();AsymmetricCounts();CellsAndOverflow();std::cout<<"PASS "<<checks<<" measurement-statistics checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
