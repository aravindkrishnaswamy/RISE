#include "pch.h"
#include "HyLIoSLocalResponse.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace RISE::HyLIoS {
namespace {

std::size_t Add(std::size_t a, std::size_t b)
{
    if (b > std::numeric_limits<std::size_t>::max() - a)
        throw std::length_error("HyLIoS local response size addition overflow");
    return a + b;
}

std::size_t Multiply(std::size_t a, std::size_t b)
{
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
        throw std::length_error("HyLIoS local response size multiplication overflow");
    return a * b;
}

void ValidateAxis(const std::vector<double>& edges, double end)
{
    if (edges.size() < 2 || edges.front() != 0 || edges.back() != end)
        throw std::invalid_argument("HyLIoS local grid must cover its complete angular domain");
    for (std::size_t k = 0; k < edges.size(); ++k) {
        if (!std::isfinite(edges[k]) || (k > 0 &&
            (!(edges[k] > edges[k-1]) || !(std::nextafter(edges[k-1], edges[k]) < edges[k]))))
            throw std::invalid_argument("HyLIoS local grid needs finite increasing edges with representable interiors");
    }
}

std::size_t FindAxis(const std::vector<double>& edges, double x)
{
    // Half-open cells, except t=1 belongs to the last polar cell.
    if (x == edges.back()) return edges.size() - 2;
    return static_cast<std::size_t>(std::upper_bound(edges.begin(), edges.end(), x) - edges.begin() - 1);
}

std::size_t FindCell(const LocalResponseGrid& grid, LocalResponsePoint p)
{
    return FindAxis(grid.tEdges, p.t) * (grid.phiEdges.size()-1) + FindAxis(grid.phiEdges, p.phi);
}

double Product(double a, double b)
{
    const double result = a*b;
    if (!std::isfinite(result) || (a > 0 && b > 0 && result == 0))
        throw std::overflow_error("HyLIoS local mass product is not representable");
    return result;
}

double Sum(double a, double b)
{
    const double result = a+b;
    if (!std::isfinite(result) || (a > 0 && b > 0 && (result == a || result == b)))
        throw std::overflow_error("HyLIoS local mass sum loses positive support");
    return result;
}

struct ExactProduct { std::uint64_t low, high; int exponent; };
struct BinaryNumber { std::uint64_t mantissa; int exponent; };

BinaryNumber Binary(double value)
{
    int exponent=0;
    const double mantissa=std::frexp(value,&exponent);
    auto integer=static_cast<std::uint64_t>(std::ldexp(mantissa,53));
    exponent-=53;
    while ((integer&1U)==0) { integer>>=1; ++exponent; }
    return {integer,exponent};
}

ExactProduct ExactMultiply(double a, double b)
{
    const auto x=Binary(a), y=Binary(b);
    // Portable 53x53->106-bit multiplication. Odd mantissas make this product
    // canonical, so equality needs no rounding, division, or wide-FP assumption.
    constexpr std::uint64_t mask=0xffffffffU;
    const std::uint64_t p0=(x.mantissa&mask)*(y.mantissa&mask);
    const std::uint64_t p1=(x.mantissa&mask)*(y.mantissa>>32);
    const std::uint64_t p2=(x.mantissa>>32)*(y.mantissa&mask);
    const std::uint64_t p3=(x.mantissa>>32)*(y.mantissa>>32);
    const std::uint64_t middle=(p0>>32)+(p1&mask)+(p2&mask);
    return {(middle<<32)|(p0&mask),p3+(p1>>32)+(p2>>32)+(middle>>32),x.exponent+y.exponent};
}

bool EqualProducts(double a, double b, double c, double d)
{
    const auto x=ExactMultiply(a,b), y=ExactMultiply(c,d);
    return x.low==y.low && x.high==y.high && x.exponent==y.exponent;
}

// Exact positive dyadic accumulation is needed for the passivity boundary:
// summing a small continuous response into a mirror near one can lose EVERY
// individual term even when their aggregate exceeds a representable amount.
class ExactRowSum {
public:
    static ExactRowSum ConvexBlend(const ExactRowSum& primary, const ExactRowSum& secondary, double minority)
    {
        const int order=primary.Compare(secondary);
        if (minority==0 || order==0) return primary;
        ExactRowSum difference=order<0 ? secondary : primary;
        difference.Subtract(order<0 ? primary : secondary);
        const ExactRowSum correction=difference.TimesDyadic(minority);
        ExactRowSum result=primary;
        if (order<0) {
            for (std::size_t i=0;i<result.words_.size();++i) result.AddWord(correction.words_[i],i);
        } else result.Subtract(correction);
        return result;
    }

    void AddProduct(double a, double b, int exponent=0)
    {
        if (a==0 || b==0) return;
        const ExactProduct p=ExactMultiply(a,b);
        const int shift=p.exponent+exponent-BaseExponent;
        if (shift<0) throw std::overflow_error("HyLIoS exact row exponent outside proven numeric range");
        const std::size_t word=static_cast<std::size_t>(shift/64);
        const unsigned offset=static_cast<unsigned>(shift%64);
        if (offset==0) { AddWord(p.low,word); AddWord(p.high,word+1); }
        else {
            AddWord(p.low<<offset,word);
            AddWord(p.low>>(64-offset),word+1);
            AddWord(p.high<<offset,word+1);
            AddWord(p.high>>(64-offset),word+2);
        }
    }

    bool GreaterThanOne() const
    {
        if (overflow_ || words_.back()>1) return true;
        if (words_.back()==0) return false;
        for (std::size_t i=0;i+1<words_.size();++i) if (words_[i]!=0) return true;
        return false;
    }

    double Rounded() const
    {
        if (overflow_) throw std::overflow_error("HyLIoS exact row sum overflow");
        std::size_t last=words_.size();
        while (last>0 && words_[last-1]==0) --last;
        if (last==0) return 0;
        --last;
        unsigned top=0;
        std::uint64_t high=words_[last];
        while ((high>>1)!=0) { high>>=1; ++top; }
        const int highest=static_cast<int>(last*64)+static_cast<int>(top);
        // Keep 53 normal bits, or retain every representable subnormal bit.
        const int shift=std::max(highest-52,-1074-BaseExponent);
        const std::size_t word=static_cast<std::size_t>(shift/64);
        const unsigned offset=static_cast<unsigned>(shift%64);
        std::uint64_t kept=words_[word]>>offset;
        if (offset!=0 && word+1<words_.size()) kept|=words_[word+1]<<(64-offset);
        kept&=0x1fffffffffffffULL;
        const int guardIndex=shift-1;
        const auto guardWord=static_cast<std::size_t>(guardIndex/64);
        const unsigned guardOffset=static_cast<unsigned>(guardIndex%64);
        const bool guard=((words_[guardWord]>>guardOffset)&1U)!=0;
        bool sticky=false;
        for (std::size_t i=0;i<guardWord;++i) sticky=sticky || words_[i]!=0;
        if (guardOffset!=0) sticky=sticky || (words_[guardWord]&((std::uint64_t{1}<<guardOffset)-1))!=0;
        if (guard && (sticky || (kept&1U)!=0)) ++kept;
        const double result=std::scalbn(static_cast<double>(kept),BaseExponent+shift);
        if (result==0 || !std::isfinite(result))
            throw std::overflow_error("HyLIoS local physical row output is not representable");
        return result;
    }

private:
    // Validated cell areas have normalized exponent >=-1074. A binary64 F
    // followed by one spectral interpolation has lowest product bit >=-3327
    // when multiplied by such an area. These limbs cover 2^-3392 through 2^64;
    // an upper overflow is positive and necessarily violates unit passivity.
    static constexpr int BaseExponent=-3392;
    std::array<std::uint64_t,54> words_{};
    bool overflow_=false;

    int Compare(const ExactRowSum& other) const
    {
        if (overflow_ || other.overflow_) throw std::overflow_error("HyLIoS exact row comparison overflow");
        for (std::size_t i=words_.size();i>0;--i)
            if (words_[i-1]!=other.words_[i-1]) return words_[i-1]<other.words_[i-1] ? -1 : 1;
        return 0;
    }

    void Subtract(const ExactRowSum& other)
    {
        std::uint64_t borrow=0;
        for (std::size_t i=0;i<words_.size();++i) {
            const std::uint64_t old=words_[i];
            const std::uint64_t partial=old-other.words_[i];
            const bool borrowed=old<other.words_[i];
            words_[i]=partial-borrow;
            borrow=borrowed || partial<borrow ? 1 : 0;
        }
        if (borrow!=0) throw std::overflow_error("HyLIoS exact row subtraction became negative");
    }

    ExactRowSum TimesDyadic(double weight) const
    {
        ExactRowSum result;
        const BinaryNumber factor=Binary(weight);
        std::uint64_t bits=factor.mantissa;
        int bit=0;
        while (bits!=0) {
            if ((bits&1U)!=0) {
                for (std::size_t i=0;i<words_.size();++i) {
                    if (words_[i]==0) continue;
                    const int shift=static_cast<int>(i*64)+factor.exponent+bit;
                    if (shift<0) throw std::overflow_error("HyLIoS exact convex row below proven exponent range");
                    const auto word=static_cast<std::size_t>(shift/64);
                    const auto offset=static_cast<unsigned>(shift%64);
                    result.AddWord(words_[i]<<offset,word);
                    if (offset!=0) result.AddWord(words_[i]>>(64-offset),word+1);
                }
            }
            bits>>=1; ++bit;
        }
        return result;
    }

    void AddWord(std::uint64_t value, std::size_t index)
    {
        while (value!=0) {
            if (index>=words_.size()) { overflow_=true; return; }
            const std::uint64_t old=words_[index];
            words_[index]+=value;
            value=words_[index]<old ? 1 : 0;
            ++index;
        }
    }
};

static_assert(std::numeric_limits<double>::radix==2 && std::numeric_limits<double>::digits==53 &&
    std::numeric_limits<double>::min_exponent==-1021 && std::numeric_limits<double>::max_exponent==1024,
    "HyLIoS local response exact arithmetic requires IEEE binary64");

void ValidateDraw(double draw, bool open)
{
    if (!std::isfinite(draw) || !(draw < 1) || (open ? !(draw > 0) : !(draw >= 0)))
        throw std::invalid_argument("HyLIoS local random draw outside required unit interval");
}

double MinimumInterval(LocalResponseSamplingGrid grid)
{
    if (grid.precisionBits==0 || grid.precisionBits>53)
        throw std::invalid_argument("HyLIoS local discrete sampling grid must have 1..53 bits");
    const double step=std::scalbn(1.0,-static_cast<int>(grid.precisionBits));
    // Two lattice steps leave an interior lattice point after removing the
    // normalized binary64 product error (<=2^-53) from each interval endpoint.
    return 2*step+0x1p-52;
}

void CheckInterval(double probability, double minimum)
{
    // Called only for positive active intervals. A deterministic branch needs
    // no random selection interval; a rounded zero must not masquerade as absent.
    if (!(probability>0) || (probability<1 && probability<minimum))
        throw std::domain_error("HyLIoS local proposal interval is below declared RNG-grid sampling support");
}

double Interior(double a, double b, double u)
{
    const double x = a + u*(b-a);
    // Finite-precision endpoint policy, not a physical change in grid support:
    // keep a rounded sample in the cell whose CDF selected it.
    if (x <= a) return std::nextafter(a, b);
    if (x >= b) return std::nextafter(b, a);
    return x;
}

double FloorAngleSum(double a, double b)
{
    // FastTwoSum with |a|>=|b|, for bounded angles and nearest/even arithmetic.
    // Materialize the operations so fast-math cannot reassociate the residual
    // to zero. Its sign says whether the rounded sum lies above the exact sum.
    const volatile double sum=a+b;
    const volatile double recovered=sum-a;
    const double residual=b-recovered;
    const double rounded=sum;
    return residual<0 ? std::nextafter(rounded,-std::numeric_limits<double>::infinity()) : rounded;
}

} // namespace

LocalResponseStorage HyLIoSLocalResponse::EstimateStorage(std::size_t nw,
    std::size_t it, std::size_t ip, std::size_t ot, std::size_t op,
    const LocalResponseLimits& limits)
{
    if (nw == 0 || it == 0 || ip == 0 || ot == 0 || op == 0)
        throw std::invalid_argument("HyLIoS local response dimensions must be positive");
    const std::size_t ni = Multiply(it, ip), no = Multiply(ot, op);
    const std::size_t nf = Multiply(Multiply(nw, ni), no);
    if (nw > limits.maximumWavelengths || ni > limits.maximumCellsPerGrid ||
        no > limits.maximumCellsPerGrid || nf > limits.maximumContinuousEntries)
        throw std::length_error("HyLIoS local response exceeds dimension/cardinality budget");
    std::size_t doubles = Add(Add(Add(Add(it, ip), ot), op), 4);
    doubles = Add(doubles, nw);
    doubles = Add(doubles, Multiply(2, nf)); // F and column CDF, same cardinality.
    doubles = Add(doubles, Multiply(nw, ni)); // mirror coefficients.
    doubles = Add(doubles, Multiply(nw,no)); // minimum CDF interval per column.
    const std::size_t flags=Multiply(nw-1,no);
    const std::size_t exponents=Multiply(Multiply(nw,no),sizeof(int));
    const std::size_t areas=Multiply(Add(ni,no),sizeof(Scaled));
    const std::size_t bytes = Add(Add(Add(Add(sizeof(HyLIoSLocalResponse), Multiply(doubles, sizeof(double))),flags),exponents),areas);
    if (bytes > limits.maximumStorageBytes)
        throw std::length_error("HyLIoS local response exceeds owned-storage budget");
    return {ni, no, nf, bytes};
}

std::shared_ptr<const HyLIoSLocalResponse> HyLIoSLocalResponse::Create(
    const LocalResponseDefinition& d, const LocalResponseLimits& limits)
{
    if (d.incident.tEdges.size() < 2 || d.incident.phiEdges.size() < 2 ||
        d.outgoing.tEdges.size() < 2 || d.outgoing.phiEdges.size() < 2)
        throw std::invalid_argument("HyLIoS local response grid is empty");
    const LocalResponseStorage storage = EstimateStorage(d.wavelengthNm.size(),
        d.incident.tEdges.size()-1, d.incident.phiEdges.size()-1,
        d.outgoing.tEdges.size()-1, d.outgoing.phiEdges.size()-1, limits);
    if (d.continuous.size() != storage.continuousEntries ||
        d.mirror.size() != Multiply(d.wavelengthNm.size(), storage.incidentCells))
        throw std::invalid_argument("HyLIoS local response coefficient extent mismatch");
    ValidateAxis(d.incident.tEdges, 1);
    ValidateAxis(d.incident.phiEdges, LocalResponseTwoPi);
    ValidateAxis(d.outgoing.tEdges, 1);
    ValidateAxis(d.outgoing.phiEdges, LocalResponseTwoPi);
    for (std::size_t l = 0; l < d.wavelengthNm.size(); ++l) {
        if (!std::isfinite(d.wavelengthNm[l]) || !(d.wavelengthNm[l] > 0) ||
            (l > 0 && !(d.wavelengthNm[l] > d.wavelengthNm[l-1])))
            throw std::invalid_argument("HyLIoS local wavelengths must be positive finite increasing nodes");
    }
    for (double value : d.continuous)
        if (!std::isfinite(value) || value < 0)
            throw std::invalid_argument("HyLIoS local continuous coefficient must be finite nonnegative");
    for (double value : d.mirror)
        if (!std::isfinite(value) || value < 0)
            throw std::invalid_argument("HyLIoS local mirror coefficient must be finite nonnegative");
    return std::shared_ptr<const HyLIoSLocalResponse>(new HyLIoSLocalResponse(d, storage));
}

HyLIoSLocalResponse::HyLIoSLocalResponse(const LocalResponseDefinition& d, LocalResponseStorage storage)
    : storage_(storage), incident_(d.incident), outgoing_(d.outgoing),
      wavelengths_(d.wavelengthNm), continuous_(d.continuous), mirror_(d.mirror)
{
    incidentAreas_.reserve(storage_.incidentCells);
    outgoingAreas_.reserve(storage_.outgoingCells);
    for (std::size_t i = 0; i < storage_.incidentCells; ++i) incidentAreas_.push_back(ProjectedArea(incident_, i));
    for (std::size_t j = 0; j < storage_.outgoingCells; ++j) outgoingAreas_.push_back(ProjectedArea(outgoing_, j));
    for (std::size_t l = 0; l < wavelengths_.size(); ++l) {
        for (std::size_t i = 0; i < storage_.incidentCells; ++i) {
            ExactRowSum row;
            row.AddProduct(mirror_[l*storage_.incidentCells+i],1);
            for (std::size_t j = 0; j < storage_.outgoingCells; ++j)
                row.AddProduct(F(l,i,j),outgoingAreas_[j].value,outgoingAreas_[j].exponent);
            // Compare exact represented products, before physical-output rounding.
            // No energy correction, tolerance or probability normalization.
            if (row.GreaterThanOne())
                throw std::invalid_argument("HyLIoS local forward row violates passivity (continuous + mirror > 1)");
        }
    }
    columnCdf_.resize(storage_.continuousEntries);
    columnExponent_.resize(wavelengths_.size()*storage_.outgoingCells,0);
    minimumCellProbability_.resize(wavelengths_.size()*storage_.outgoingCells,1);
    for (std::size_t l = 0; l < wavelengths_.size(); ++l) {
        for (std::size_t j = 0; j < storage_.outgoingCells; ++j) {
            Scaled maximum=MakeScaled(0);
            for (std::size_t i=0;i<storage_.incidentCells;++i) {
                const Scaled mass=MultiplyScaled(MakeScaled(F(l,i,j)),incidentAreas_[i]);
                if (LessScaled(maximum,mass)) maximum=mass;
            }
            const int exponent=maximum.exponent;
            columnExponent_[l*storage_.outgoingCells+j]=exponent;
            double cumulative = 0;
            double smallest=std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < storage_.incidentCells; ++i) {
                const double before=cumulative;
                const Scaled mass=MultiplyScaled(MakeScaled(F(l,i,j)),incidentAreas_[i]);
                const double scaled=std::scalbn(mass.value,mass.exponent-exponent);
                if (mass.value>0 && scaled==0)
                    throw std::overflow_error("HyLIoS local normalized CDF mass is not representable");
                cumulative = Sum(cumulative,scaled);
                if (cumulative>before) smallest=std::min(smallest,cumulative-before);
                columnCdf_[(l*storage_.outgoingCells+j)*storage_.incidentCells+i] = cumulative;
            }
            if (cumulative>0) minimumCellProbability_[l*storage_.outgoingCells+j]=smallest/cumulative;
        }
    }
    sameAdjacentColumn_.resize((wavelengths_.size()-1)*storage_.outgoingCells,1);
    for (std::size_t l=0;l+1<wavelengths_.size();++l) {
        for (std::size_t j=0;j<storage_.outgoingCells;++j) {
            double ref0=0,ref1=0;
            for (std::size_t i=0;i<storage_.incidentCells;++i) {
                const double f0=F(l,i,j), f1=F(l+1,i,j);
                if ((f0==0)!=(f1==0)) { sameAdjacentColumn_[l*storage_.outgoingCells+j]=0; break; }
                if (f0==0) continue;
                if (ref0==0) { ref0=f0; ref1=f1; }
                else if (!EqualProducts(f0,ref1,f1,ref0)) {
                    sameAdjacentColumn_[l*storage_.outgoingCells+j]=0; break;
                }
            }
        }
    }
}

bool HyLIoSLocalResponse::Covers(double first, double last) const noexcept
{
    return std::isfinite(first) && std::isfinite(last) && first <= last &&
        first >= wavelengths_.front() && last <= wavelengths_.back();
}

void HyLIoSLocalResponse::RequireCoverage(double first, double last) const
{
    if (!Covers(first,last))
        throw std::out_of_range("HyLIoS local response does not cover the complete requested wavelength interval");
}

HyLIoSLocalResponse::SpectralBracket HyLIoSLocalResponse::Bracket(double nm) const
{
    RequireCoverage(nm,nm);
    const auto found = std::lower_bound(wavelengths_.begin(), wavelengths_.end(), nm);
    const std::size_t hi = static_cast<std::size_t>(found-wavelengths_.begin());
    if (*found == nm) return {hi,hi,0,true};
    const std::size_t lo = hi-1;
    const double distanceLo=nm-wavelengths_[lo], distanceHi=wavelengths_[hi]-nm;
    const bool primaryLo=distanceLo<=distanceHi;
    const Scaled totalDistance=AddScaled(MakeScaled(distanceLo),MakeScaled(distanceHi));
    const double minority=RatioScaled(MakeScaled(primaryLo ? distanceLo : distanceHi),totalDistance);
    if (!(minority>0) || minority>.5)
        throw std::overflow_error("HyLIoS local spectral interpolation fraction is not representable");
    // ONE dyadic coordinate defines the physical interpolation. Its complement
    // is exact 1-minority, never a second independently rounded stored weight.
    return {lo,hi,minority,primaryLo};
}

double HyLIoSLocalResponse::Interpolate(double lo, double hi, const SpectralBracket& s) const
{
    return Physical(InterpolateScaled(MakeScaled(lo),MakeScaled(hi),s));
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::MakeScaled(double value, int exponent)
{
    if (value==0) return {0,0};
    int shift=0;
    const double mantissa=std::frexp(value,&shift);
    return {mantissa,exponent+shift};
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::TimesScaled(Scaled a, double b)
{
    return MultiplyScaled(a,MakeScaled(b));
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::ComplementTimesScaled(Scaled a, double minority)
{
    if (a.value==0 || minority==0) return a;
    return MakeScaled(std::fma(-minority,a.value,a.value),a.exponent);
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::MultiplyScaled(Scaled a, Scaled b)
{
    if (a.value==0 || b.value==0) return {0,0};
    return MakeScaled(a.value*b.value,a.exponent+b.exponent);
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::ProjectedArea(const LocalResponseGrid& grid, std::size_t i)
{
    const std::size_t np=grid.phiEdges.size()-1, ti=i/np, pi=i%np;
    const Scaled area=TimesScaled(TimesScaled(MakeScaled(grid.tEdges[ti+1]-grid.tEdges[ti]),
        grid.phiEdges[pi+1]-grid.phiEdges[pi]),.5);
    // Keep the original public-grid contract: scalar cell areas must be
    // representable, including positive subnormals. Computations retain scale.
    Physical(area);
    return area;
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::AddScaled(Scaled a, Scaled b)
{
    if (a.value==0) return b;
    if (b.value==0) return a;
    const int exponent=std::max(a.exponent,b.exponent);
    // Only the normalized sum rounds here. Components retain their exponents
    // separately for branch probabilities; a subnormal physical scale is never
    // materialized as an intermediate multiplication or sum.
    return MakeScaled(std::scalbn(a.value,a.exponent-exponent)+
        std::scalbn(b.value,b.exponent-exponent),exponent);
}

bool HyLIoSLocalResponse::LessScaled(Scaled a, Scaled b)
{
    if (a.value==0) return b.value>0;
    if (b.value==0) return false;
    return a.exponent!=b.exponent ? a.exponent<b.exponent : a.value<b.value;
}

double HyLIoSLocalResponse::Physical(Scaled a)
{
    const double result=std::scalbn(a.value,a.exponent);
    if (!std::isfinite(result) || (a.value>0 && result==0))
        throw std::overflow_error("HyLIoS local physical output is not representable");
    return result;
}

double HyLIoSLocalResponse::RatioScaled(Scaled a, Scaled b)
{
    if (a.value==0) return 0;
    if (b.value==0) throw std::overflow_error("HyLIoS local positive ratio has zero denominator");
    return Physical(MakeScaled(a.value/b.value,a.exponent-b.exponent));
}

bool HyLIoSLocalResponse::ChooseFirstScaled(Scaled a, Scaled b, double draw)
{
    if (b.value==0) return true;
    if (a.value==0) return false;
    const bool firstIsRare=!LessScaled(b,a);
    const double rare=RatioScaled(firstIsRare ? a : b,AddScaled(a,b));
    return firstIsRare ? draw<rare : draw>=rare;
}

void HyLIoSLocalResponse::CheckScaledInterval(Scaled part, Scaled whole, double minimum)
{
    if (LessScaled(part,TimesScaled(whole,minimum)))
        throw std::domain_error("HyLIoS local proposal interval is below declared RNG-grid sampling support");
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::InterpolateScaled(Scaled lo, Scaled hi, const SpectralBracket& s)
{
    if (lo.value==hi.value && lo.exponent==hi.exponent) return lo;
    const Scaled primary=s.primaryLo ? lo : hi, secondary=s.primaryLo ? hi : lo;
    if (s.minority==0) return primary;
    const int exponent=std::max(primary.exponent,secondary.exponent);
    const double p=std::scalbn(primary.value,primary.exponent-exponent);
    const double q=std::scalbn(secondary.value,secondary.exponent-exponent);
    const double value=std::fma(s.minority,q-p,p);
    if (value>=std::numeric_limits<double>::min()) return MakeScaled(value,exponent);
    // A common scale can put the RESULT in the subnormal range even when its
    // eventual physical value is normal. Retain separate weighted scales then.
    return AddScaled(ComplementTimesScaled(primary,s.minority),TimesScaled(secondary,s.minority));
}

double HyLIoSLocalResponse::StoredContinuous(std::size_t node,std::size_t incident,std::size_t outgoing) const
{
    if(node>=wavelengths_.size()||incident>=storage_.incidentCells||outgoing>=storage_.outgoingCells)
        throw std::out_of_range("HyLIoS stored continuous coefficient index");
    return F(node,incident,outgoing);
}
double HyLIoSLocalResponse::StoredMirror(std::size_t node,std::size_t incident) const
{
    if(node>=wavelengths_.size()||incident>=storage_.incidentCells)
        throw std::out_of_range("HyLIoS stored mirror coefficient index");
    return mirror_[node*storage_.incidentCells+incident];
}

double HyLIoSLocalResponse::F(std::size_t l, std::size_t i, std::size_t j) const
{
    return continuous_[(l*storage_.incidentCells+i)*storage_.outgoingCells+j];
}

double HyLIoSLocalResponse::ColumnCdfTotal(std::size_t l, std::size_t j) const
{
    return columnCdf_[(l*storage_.outgoingCells+j+1)*storage_.incidentCells-1];
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::ScaledColumn(std::size_t l, std::size_t j) const
{
    return MakeScaled(ColumnCdfTotal(l,j),columnExponent_[l*storage_.outgoingCells+j]);
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::ContinuousMass(std::size_t j, const SpectralBracket& s) const
{
    return InterpolateScaled(ScaledColumn(s.lo,j),ScaledColumn(s.hi,j),s);
}

HyLIoSLocalResponse::CameraMixture HyLIoSLocalResponse::SamplingMixture(LocalResponsePoint wo,
    const SpectralBracket& s, LocalResponseSamplingGrid grid) const
{
    const double minimum=MinimumInterval(grid);
    const std::size_t j=OutgoingCell(wo);
    const Scaled c=ContinuousMass(j,s), d=Mirror(wo,s), z=AddScaled(c,d);
    const bool same=s.lo==s.hi || sameAdjacentColumn_[s.lo*storage_.outgoingCells+j]!=0;
    // Proportional physical columns have the same normalized angular law and
    // need no artificial latent spectral choice, even near a wavelength node.
    const Scaled c0=same ? c : (s.primaryLo ? ComplementTimesScaled(ScaledColumn(s.lo,j),s.minority) :
        TimesScaled(ScaledColumn(s.lo,j),s.minority));
    const Scaled c1=same ? MakeScaled(0) : (s.primaryLo ? TimesScaled(ScaledColumn(s.hi,j),s.minority) :
        ComplementTimesScaled(ScaledColumn(s.hi,j),s.minority));
    if (c.value>0 && d.value>0) CheckScaledInterval(LessScaled(c,d) ? c : d,z,minimum);
    const Scaled nodeMass=AddScaled(c0,c1);
    if (c0.value>0 && c1.value>0) CheckScaledInterval(LessScaled(c0,c1) ? c0 : c1,nodeMass,minimum);
    if (c0.value>0) CheckInterval(minimumCellProbability_[s.lo*storage_.outgoingCells+j],minimum);
    if (c1.value>0) CheckInterval(minimumCellProbability_[s.hi*storage_.outgoingCells+j],minimum);
    return {j,c0,c1,c,d,z,same};
}

void HyLIoSLocalResponse::RequireSamplingSupport(LocalResponsePoint wo, double nm, LocalResponseSamplingGrid grid) const
{
    const auto s=Bracket(nm);
    SamplingMixture(CanonicalPoint(wo),s,grid);
}

HyLIoSLocalResponse::Scaled HyLIoSLocalResponse::Mirror(LocalResponsePoint wo, const SpectralBracket& s) const
{
    const std::size_t i = IncidentCell(PlanarMirror(wo));
    return InterpolateScaled(MakeScaled(mirror_[s.lo*storage_.incidentCells+i]), MakeScaled(mirror_[s.hi*storage_.incidentCells+i]), s);
}

LocalResponsePoint HyLIoSLocalResponse::CanonicalPoint(LocalResponsePoint p)
{
    if (!std::isfinite(p.t) || !std::isfinite(p.phi) || !(p.t > 0) || p.t > 1)
        throw std::invalid_argument("HyLIoS local direction must be finite and in the open outward hemisphere");
    if (p.t == 1) return {1,0};
    p.phi = std::fmod(p.phi,LocalResponseTwoPi);
    if (p.phi < 0) p.phi=FloorAngleSum(LocalResponseTwoPi,p.phi);
    return p;
}

LocalResponsePoint HyLIoSLocalResponse::PlanarMirror(LocalResponsePoint p)
{
    p = CanonicalPoint(p);
    if (p.t==1) return p;
    // Upper-half subtraction is exact by Sterbenz. Lower-half addition may
    // round up across ANY authored binary64 cell edge: use its downward
    // representative, which stays in the cell of the exact dyadic mirror map.
    p.phi=p.phi>=LocalResponsePi ? p.phi-LocalResponsePi : FloorAngleSum(LocalResponsePi,p.phi);
    return p;
}

LocalResponseDirection HyLIoSLocalResponse::ToDirection(LocalResponsePoint p)
{
    p = CanonicalPoint(p);
    const double r = std::sqrt(1-p.t);
    return {r*std::cos(p.phi), r*std::sin(p.phi), std::sqrt(p.t)};
}

LocalResponsePoint HyLIoSLocalResponse::FromDirection(LocalResponseDirection d)
{
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) || !(d.z > 0))
        throw std::invalid_argument("HyLIoS local Cartesian direction must be finite and outward");
    // Scale first: even the norm of a finite DBL_MAX vector can overflow.
    const double scale = std::max({std::abs(d.x),std::abs(d.y),d.z});
    // Exact power-of-two scaling also avoids a fast-math reciprocal of a
    // subnormal scale overflowing before the otherwise harmless normalization.
    const int exponent=std::ilogb(scale);
    const double x=std::scalbn(d.x,-exponent), y=std::scalbn(d.y,-exponent), z=std::scalbn(d.z,-exponent);
    const double mu = z/std::sqrt(x*x+y*y+z*z);
    double phi=std::atan2(y,x);
    // Retain known quadrant sides when atan2/scaling rounds a nonzero angular
    // component onto an axis. Exact signed-zero components remain on the axis.
    if (d.y<0 && d.x>0 && phi==0) phi=std::nextafter(LocalResponseTwoPi,0.0);
    else if (d.y>0 && d.x<0 && phi==LocalResponsePi) phi=std::nextafter(LocalResponsePi,0.0);
    return CanonicalPoint({Product(mu,mu),phi});
}

std::size_t HyLIoSLocalResponse::IncidentCell(LocalResponsePoint p) const
{
    return FindCell(incident_,CanonicalPoint(p));
}

std::size_t HyLIoSLocalResponse::OutgoingCell(LocalResponsePoint p) const
{
    return FindCell(outgoing_,CanonicalPoint(p));
}

double HyLIoSLocalResponse::IncidentCellArea(std::size_t i) const
{
    return Physical(incidentAreas_.at(i));
}

double HyLIoSLocalResponse::OutgoingCellArea(std::size_t j) const
{
    return Physical(outgoingAreas_.at(j));
}

double HyLIoSLocalResponse::EvaluateContinuousNM(LocalResponsePoint wi, LocalResponsePoint wo, double nm) const
{
    const auto s = Bracket(nm);
    const std::size_t i=IncidentCell(wi), j=OutgoingCell(wo);
    return Interpolate(F(s.lo,i,j),F(s.hi,i,j),s);
}

double HyLIoSLocalResponse::ForwardRowMassNM(LocalResponsePoint wi, double nm) const
{
    const auto s = Bracket(nm);
    const std::size_t i=IncidentCell(wi);
    ExactRowSum lo,hi;
    lo.AddProduct(mirror_[s.lo*storage_.incidentCells+i],1);
    hi.AddProduct(mirror_[s.hi*storage_.incidentCells+i],1);
    for (std::size_t j=0; j<storage_.outgoingCells; ++j) {
        lo.AddProduct(F(s.lo,i,j),outgoingAreas_[j].value,outgoingAreas_[j].exponent);
        hi.AddProduct(F(s.hi,i,j),outgoingAreas_[j].value,outgoingAreas_[j].exponent);
    }
    return ExactRowSum::ConvexBlend(s.primaryLo ? lo : hi,s.primaryLo ? hi : lo,s.minority).Rounded();
}

double HyLIoSLocalResponse::CameraContinuousMassNM(LocalResponsePoint wo, double nm) const
{
    const auto s = Bracket(nm);
    return Physical(ContinuousMass(OutgoingCell(wo),s));
}

double HyLIoSLocalResponse::CameraMirrorMassNM(LocalResponsePoint wo, double nm) const
{
    const auto s = Bracket(nm);
    return Physical(Mirror(wo,s));
}

double HyLIoSLocalResponse::CameraPdfNM(LocalResponsePoint wi, LocalResponsePoint wo, double nm) const
{
    const auto s = Bracket(nm);
    wi = CanonicalPoint(wi);
    const std::size_t i=IncidentCell(wi), j=OutgoingCell(wo);
    const Scaled z=AddScaled(ContinuousMass(j,s),Mirror(wo,s));
    if (z.value==0) return 0;
    const Scaled f=InterpolateScaled(MakeScaled(F(s.lo,i,j)),MakeScaled(F(s.hi,i,j)),s);
    return RatioScaled(TimesScaled(f,std::sqrt(wi.t)),z);
}

std::size_t HyLIoSLocalResponse::SelectCell(std::size_t l, std::size_t j, double draw) const
{
    const auto begin=columnCdf_.begin()+static_cast<std::ptrdiff_t>((l*storage_.outgoingCells+j)*storage_.incidentCells);
    const auto end=begin+static_cast<std::ptrdiff_t>(storage_.incidentCells);
    double target=draw*ColumnCdfTotal(l,j);
    // Multiplication may round [0,1)*C to C. Keep it below the final endpoint.
    if (target >= ColumnCdfTotal(l,j)) target=std::nextafter(ColumnCdfTotal(l,j),0.0);
    const auto selected=std::upper_bound(begin,end,target);
    if (selected == end) throw std::overflow_error("HyLIoS local CDF selection failed");
    return static_cast<std::size_t>(selected-begin);
}

LocalResponsePoint HyLIoSLocalResponse::SampleInCell(std::size_t i, double u, double v) const
{
    const std::size_t np=incident_.phiEdges.size()-1, ti=i/np, pi=i%np;
    return {Interior(incident_.tEdges[ti],incident_.tEdges[ti+1],u),
        Interior(incident_.phiEdges[pi],incident_.phiEdges[pi+1],v)};
}

LocalResponseSample HyLIoSLocalResponse::SampleCameraNM(LocalResponsePoint wo, double nm,
    double event, double node, double cell, double projectedT, double azimuth,
    LocalResponseSamplingGrid grid) const
{
    ValidateDraw(event,false); ValidateDraw(node,false); ValidateDraw(cell,false);
    ValidateDraw(projectedT,true); ValidateDraw(azimuth,true);
    const auto s=Bracket(nm);
    wo=CanonicalPoint(wo);
    const auto mixture=SamplingMixture(wo,s,grid);
    if (mixture.z.value == 0) return {};
    LocalResponseSample result;
    result.weight=Physical(mixture.z);
    // Select spectral NODE with its unnormalized physical column mass. Using
    // spectral interpolation weights on normalized nodal PDFs would be biased.
    if (ChooseFirstScaled(mixture.c,mixture.d,event)) {
        // A fresh independent node variate preserves the node's full resolution
        // even after a rare continuous event. Never remap its event variate.
        const std::size_t l=mixture.sameColumn || ChooseFirstScaled(mixture.c0,mixture.c1,node) ? s.lo : s.hi;
        result.event=LocalResponseSample::Event::Continuous;
        result.incident=SampleInCell(SelectCell(l,mixture.j,cell),projectedT,azimuth);
        result.pdf=CameraPdfNM(result.incident,wo,nm);
        result.eventProbability=RatioScaled(mixture.c,mixture.z);
    } else {
        result.event=LocalResponseSample::Event::PlanarMirror;
        result.incident=PlanarMirror(wo);
        result.eventProbability=RatioScaled(mixture.d,mixture.z);
    }
    return result;
}

std::shared_ptr<const HyLIoSSupportedCameraProposal> HyLIoSSupportedCameraProposal::Create(
    std::shared_ptr<const HyLIoSLocalResponse> response, LocalResponseSamplingGrid grid)
{
    if (!response) throw std::invalid_argument("HyLIoS supported proposal requires a response");
    if (grid.precisionBits==0 || grid.precisionBits>52)
        throw std::invalid_argument("HyLIoS supported proposal midpoint grid needs 1..52 bits");
    return std::shared_ptr<const HyLIoSSupportedCameraProposal>(
        new HyLIoSSupportedCameraProposal(std::move(response),grid.precisionBits));
}

HyLIoSSupportedCameraProposal::HyLIoSSupportedCameraProposal(
    std::shared_ptr<const HyLIoSLocalResponse> response, unsigned bits)
    : response_(std::move(response)), bits_(bits)
{
    using Core=HyLIoSLocalResponse;
    const auto& r=*response_;
    const auto n=r.storage_.incidentCells;
    if ((n&(n-1))!=0)
        throw std::domain_error("HyLIoS supported proposal requires a power-of-two incident cell count");
    const double count=static_cast<double>(n);
    const double step=std::scalbn(1.0,-static_cast<int>(bits_));
    // Midpoint gaps must exceed four ulps at the upper edge. A rounded width,
    // one multiplication and one addition/FMA then cannot merge adjacent
    // midpoint samples or move an extreme midpoint to a cell endpoint.
    for (const auto* edges:{&r.incident_.tEdges,&r.incident_.phiEdges}) {
        for (std::size_t k=0;k+1<edges->size();++k) {
            const double a=(*edges)[k], b=(*edges)[k+1], width=b-a;
            const auto gap=Core::TimesScaled(Core::MakeScaled(width),step);
            const auto error=Core::TimesScaled(Core::MakeScaled(std::nextafter(b,
                std::numeric_limits<double>::infinity())-b),4);
            if (Core::LessScaled(gap,error) || !(std::fma(.5*step,width,a)>a) ||
                !(std::fma(1-.5*step,width,a)<b))
                throw std::domain_error("HyLIoS supported proposal cell cannot resolve midpoint sampling grid");
        }
    }
    const auto minimum=Core::MakeScaled(std::numeric_limits<double>::denorm_min());
    const auto maximum=Core::MakeScaled(std::numeric_limits<double>::max());
    auto representable=[&](Core::Scaled low, Core::Scaled high) {
        // Factor-two outer envelopes deliberately exceed the accumulated
        // normalized binary64 rounding errors; they do not change the law.
        if ((low.value>0 && Core::LessScaled(Core::TimesScaled(low,.5),minimum)) ||
            Core::LessScaled(maximum,Core::TimesScaled(high,2)))
            throw std::domain_error("HyLIoS supported proposal scalar output range is unsupported");
    };
    for (std::size_t i=0;i<n;++i) {
        const auto denominator=Core::TimesScaled(r.incidentAreas_[i],2*count);
        const auto reciprocal=Core::MakeScaled(1/denominator.value,-denominator.exponent);
        representable(Core::TimesScaled(reciprocal,std::sqrt(std::numeric_limits<double>::denorm_min())),reciprocal);
    }
    for (std::size_t l=0;l<r.wavelengths_.size();++l) {
        double minority=1;
        if (l+1<r.wavelengths_.size()) {
            const double lo=r.wavelengths_[l], hi=r.wavelengths_[l+1];
            // Positive binary64 spacing never decreases. Every interior query
            // is at least this far from either endpoint. The computed sum of
            // distances is below twice the rounded span. Another factor two
            // leaves a lower envelope for the rounded interpolation minority.
            const auto span=Core::TimesScaled(Core::MakeScaled(hi-lo),4);
            const auto spacing=Core::MakeScaled(std::nextafter(lo,hi)-lo);
            const auto lower=Core::MakeScaled(spacing.value/span.value,spacing.exponent-span.exponent);
            if (Core::LessScaled(lower,Core::MakeScaled(std::numeric_limits<double>::min())))
                throw std::domain_error("HyLIoS supported proposal spectral coordinate range is unsupported");
            minority=Core::Physical(lower);
        }
        auto coefficient=[&](double a, double b, Core::Scaled scale) {
            const double high=std::max(a,b);
            if (high==0) return;
            const auto upper=Core::TimesScaled(scale,high);
            const auto lower=a>0 && b>0 ? Core::TimesScaled(scale,std::min(a,b)) :
                Core::TimesScaled(Core::TimesScaled(scale,high),minority);
            representable(lower,upper);
        };
        const std::size_t next=std::min(l+1,r.wavelengths_.size()-1);
        for (std::size_t i=0;i<n;++i) {
            coefficient(r.mirror_[l*n+i],r.mirror_[next*n+i],Core::MakeScaled(2));
            const auto scale=Core::TimesScaled(r.incidentAreas_[i],2*count);
            for (std::size_t j=0;j<r.storage_.outgoingCells;++j)
                coefficient(r.F(l,i,j),r.F(next,i,j),scale);
        }
    }
}

double HyLIoSSupportedCameraProposal::CameraPdfNM(LocalResponsePoint wi,
    LocalResponsePoint wo, double nm) const
{
    using Core=HyLIoSLocalResponse;
    const auto& r=*response_;
    r.RequireCoverage(nm,nm);
    Core::CanonicalPoint(wo);
    wi=Core::CanonicalPoint(wi);
    const auto denominator=Core::TimesScaled(r.incidentAreas_[r.IncidentCell(wi)],
        2*static_cast<double>(r.storage_.incidentCells));
    return Core::RatioScaled(Core::MakeScaled(std::sqrt(wi.t)),denominator);
}

LocalResponseSample HyLIoSSupportedCameraProposal::SampleCameraNM(LocalResponsePoint wo,
    double nm, unsigned event, std::size_t cell, std::uint64_t u, std::uint64_t v) const
{
    using Core=HyLIoSLocalResponse;
    const auto& r=*response_;
    const std::uint64_t cardinality=std::uint64_t{1}<<bits_;
    if (event>1 || cell>=r.storage_.incidentCells || u>=cardinality || v>=cardinality)
        throw std::invalid_argument("HyLIoS supported proposal integer draw outside declared range");
    const auto s=r.Bracket(nm);
    wo=Core::CanonicalPoint(wo);
    LocalResponseSample result;
    result.eventProbability=.5;
    if (event==0) {
        const double ut=std::scalbn(static_cast<double>(u)+.5,-static_cast<int>(bits_));
        const double vp=std::scalbn(static_cast<double>(v)+.5,-static_cast<int>(bits_));
        const auto np=r.incident_.phiEdges.size()-1, ti=cell/np, pi=cell%np;
        result.incident={std::fma(ut,r.incident_.tEdges[ti+1]-r.incident_.tEdges[ti],r.incident_.tEdges[ti]),
            std::fma(vp,r.incident_.phiEdges[pi+1]-r.incident_.phiEdges[pi],r.incident_.phiEdges[pi])};
        result.event=LocalResponseSample::Event::Continuous;
        result.pdf=CameraPdfNM(result.incident,wo,nm);
        const auto j=r.OutgoingCell(wo);
        const auto f=Core::InterpolateScaled(Core::MakeScaled(r.F(s.lo,cell,j)),Core::MakeScaled(r.F(s.hi,cell,j)),s);
        result.weight=Core::Physical(Core::MultiplyScaled(f,Core::TimesScaled(r.incidentAreas_[cell],
            2*static_cast<double>(r.storage_.incidentCells))));
    } else {
        result.event=LocalResponseSample::Event::PlanarMirror;
        result.incident=Core::PlanarMirror(wo);
        result.weight=Core::Physical(Core::TimesScaled(r.Mirror(wo,s),2));
    }
    return result;
}

} // namespace RISE::HyLIoS
