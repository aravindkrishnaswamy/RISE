//////////////////////////////////////////////////////////////////////
//
//  RadiometricExactResolve.h - Private exact IEEE binary64 nearest-even
//    rounding of compensated radiometric moments (high+low)*2^power/(a*b).
//    Uses a fixed-capacity base-2^32 UInt with no external bignum dependency.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_RADIOMETRIC_EXACT_RESOLVE_H
#define RISE_RADIOMETRIC_EXACT_RESOLVE_H
#include "../Utilities/FiniteMath.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace RISE { namespace Implementation { namespace RadiometricExact {
static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559 &&
    std::numeric_limits<double>::radix==2 && std::numeric_limits<double>::digits==53 &&
    std::numeric_limits<double>::min_exponent==-1021 && std::numeric_limits<double>::max_exponent==1024,
    "radiometric exact resolution requires IEEE binary64");
// A binary64 high/low dyadic numerator spans at most 2098 bits; two exact
// count factors <=2^53 add at most 107 integer bits. This bound also covers shifted
// denominator comparisons during the final 53-bit quotient extraction.
constexpr std::size_t wordCount=(std::numeric_limits<double>::max_exponent-
    std::numeric_limits<double>::min_exponent+3*std::numeric_limits<double>::digits+3+31)/32;
constexpr int trueMinExponent=std::numeric_limits<double>::min_exponent-std::numeric_limits<double>::digits;
constexpr int halfMinExponent=trueMinExponent-1;
// Fixed-width little-endian base-2^32 unsigned integer sized to hold exact
// shifted binary64 dyadic numerators and products of <= 2^53 sample counts.
class UInt {
    std::array<std::uint32_t,wordCount> words{};
    std::size_t used=0;
    void Trim() { while(used && words[used-1]==0) --used; }
public:
    explicit UInt(std::uint64_t value=0) {
        words[0]=static_cast<std::uint32_t>(value); words[1]=static_cast<std::uint32_t>(value>>32);
        used=words[1] ? 2 : (words[0] ? 1 : 0);
    }
    bool Zero() const { return used==0; }
    int Bits() const {
        if(!used) return 0;
        std::uint32_t high=words[used-1]; int bits=0;
        while(high) { ++bits; high>>=1; }
        return static_cast<int>((used-1)*32)+bits;
    }
    int Compare(const UInt& other) const {
        if(used!=other.used) return used<other.used ? -1 : 1;
        for(std::size_t i=used;i>0;--i) if(words[i-1]!=other.words[i-1]) return words[i-1]<other.words[i-1] ? -1 : 1;
        return 0;
    }
    bool Shift(unsigned int shift) {
        if(!used || !shift) return true;
        const std::size_t whole=shift/32; const unsigned int bits=shift%32;
        if(whole>=wordCount || used>wordCount-whole-(bits ? 1u : 0u)) return false;
        std::array<std::uint32_t,wordCount> result{};
        for(std::size_t i=0;i<used;++i) {
            const std::uint64_t value=static_cast<std::uint64_t>(words[i])<<bits;
            result[i+whole]|=static_cast<std::uint32_t>(value);
            if(bits) result[i+whole+1]|=static_cast<std::uint32_t>(value>>32);
        }
        words=result; used+=whole+(bits ? 1u : 0u); Trim(); return true;
    }
    bool Add(const UInt& other) {
        const auto length=used>other.used ? used : other.used;
        if(length>=wordCount) return false;
        std::uint64_t carry=0;
        for(std::size_t i=0;i<length;++i) {
            const std::uint64_t sum=static_cast<std::uint64_t>(words[i])+other.words[i]+carry;
            words[i]=static_cast<std::uint32_t>(sum); carry=sum>>32;
        }
        words[length]=static_cast<std::uint32_t>(carry); used=length+(carry ? 1u : 0u); return true;
    }
    bool Subtract(const UInt& other) {
        if(Compare(other)<0) return false;
        std::uint64_t borrow=0;
        for(std::size_t i=0;i<used;++i) {
            const std::uint64_t subtract=static_cast<std::uint64_t>(other.words[i])+borrow;
            const auto old=static_cast<std::uint64_t>(words[i]);
            words[i]=static_cast<std::uint32_t>(old-subtract); borrow=old<subtract ? 1u : 0u;
        }
        Trim(); return borrow==0;
    }
    static UInt Product(std::uint64_t a,std::uint64_t b) {
        UInt result;
        const std::uint32_t left[2]={static_cast<std::uint32_t>(a),static_cast<std::uint32_t>(a>>32)};
        const std::uint32_t right[2]={static_cast<std::uint32_t>(b),static_cast<std::uint32_t>(b>>32)};
        for(std::size_t i=0;i<2;++i) {
            std::uint64_t carry=0;
            for(std::size_t j=0;j<2;++j) {
                const std::uint64_t value=static_cast<std::uint64_t>(left[i])*right[j]+result.words[i+j]+carry;
                result.words[i+j]=static_cast<std::uint32_t>(value); carry=value>>32;
            }
            result.words[i+2]=static_cast<std::uint32_t>(carry);
        }
        result.used=4; result.Trim(); return result;
    }
};
// Extracts sign, unbiased binary exponent, and 53-bit (or subnormal) integer
// significand; volatile barrier forces IEEE binary64 materialization.
inline bool Parts(double value,std::uint64_t& significand,int& exponent,bool& negative) {
    volatile double barrier=value; const double materialized=barrier;
    std::uint64_t bits=0; std::memcpy(&bits,&materialized,sizeof(bits));
    negative=(bits>>63)!=0; const auto biased=static_cast<unsigned int>((bits>>52)&0x7ffu);
    if(biased==0x7ffu) return false;
    significand=bits&0xfffffffffffffu;
    if(biased) { significand|=std::uint64_t(1)<<52; exponent=static_cast<int>(biased)-1023-52; }
    else exponent=trueMinExponent;
    return true;
}
// Aligns a nonnegative compensated binary64 pair (high + low) into an exact
// integer numerator * 2^exponent.
inline bool Numerator(double high,double low,UInt& numerator,int& exponent) {
    std::uint64_t highSignificand=0,lowSignificand=0; int highExponent=0,lowExponent=0; bool highNegative=false,lowNegative=false;
    if(!Parts(high,highSignificand,highExponent,highNegative) || !Parts(low,lowSignificand,lowExponent,lowNegative) ||
       (highNegative && highSignificand)) return false;
    exponent=lowSignificand && lowExponent<highExponent ? lowExponent : highExponent;
    UInt a(highSignificand),b(lowSignificand);
    if(!a.Shift(static_cast<unsigned int>(highExponent-exponent)) || !b.Shift(static_cast<unsigned int>(lowExponent-exponent))) return false;
    if(!(lowNegative ? a.Subtract(b) : a.Add(b))) return false;
    numerator=a; return true;
}
inline int CountBits(std::uint64_t n) { int bits=0; while(n) { ++bits; n>>=1; } return bits; }
// Returns true iff a strictly positive variance (high+low)/(n*(n-1)) exceeds
// half-TRUE_MIN so it will not underflow to zero under nearest-even rounding.
inline bool PositiveVarianceRepresentable(double high,double low,std::uint64_t n) {
    if(IsZeroDouble(high) && IsZeroDouble(low)) return true;
    int highExponent=0; std::uint64_t significand=0; bool negative=false;
    if(!Parts(high,significand,highExponent,negative) || negative || !significand) return false;
    // Normalized high+low is strictly greater than high/2. The exponent bound
    // proves the common case is above half-TRUE_MIN without quotient work.
    const int lowerExponent=highExponent+CountBits(significand)-2-CountBits(n)-CountBits(n-1);
    if(lowerExponent>=halfMinExponent) return true;
    UInt numerator; int exponent=0;
    if(!Numerator(high,low,numerator,exponent) || exponent<halfMinExponent ||
       !numerator.Shift(static_cast<unsigned int>(exponent-halfMinExponent))) return false;
    return numerator.Compare(UInt::Product(n,n-1))>0; // equality rounds to even zero
}
// Resolve (high+low)*2^power/(a*b) once, nearest-even. The supplied pair is a
// represented finite nonnegative dyadic, not an assertion about its history.
inline bool Round(double high,double low,int power,std::uint64_t a,std::uint64_t b,double& output) {
    static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559,"binary64 required");
    if(!a || !b || a>(std::uint64_t(1)<<53) || b>(std::uint64_t(1)<<53)) return false;
    UInt numerator; int exponent=0;
    if(!Numerator(high,low,numerator,exponent) || numerator.Zero()) return false;
    exponent+=power;
    const UInt denominator=UInt::Product(a,b);
    int leading=numerator.Bits()-denominator.Bits()+exponent;
    UInt comparisonNumerator=numerator,comparisonDenominator=denominator;
    const int align=leading-exponent;
    if(!(align>=0 ? comparisonDenominator.Shift(static_cast<unsigned int>(align)) : comparisonNumerator.Shift(static_cast<unsigned int>(-align)))) return false;
    if(comparisonNumerator.Compare(comparisonDenominator)<0) --leading;
    if(leading>1023 || leading<halfMinExponent) return false;
    int quantum=leading-52;
    if(quantum<trueMinExponent) quantum=trueMinExponent;
    UInt remainder=numerator,divisor=denominator;
    const int shift=exponent-quantum;
    if(!(shift>=0 ? remainder.Shift(static_cast<unsigned int>(shift)) : divisor.Shift(static_cast<unsigned int>(-shift)))) return false;
    const int quotientBits=remainder.Bits()-divisor.Bits();
    if(quotientBits>=64) return false;
    std::uint64_t quotient=0;
    for(int bit=quotientBits;bit>=0;--bit) {
        UInt term=divisor;
        if(!term.Shift(static_cast<unsigned int>(bit))) return false;
        if(remainder.Compare(term)>=0) {
            if(!remainder.Subtract(term)) return false;
            quotient|=std::uint64_t(1)<<bit;
        }
    }
    // Exact range refusal: do not saturate values just above DBL_MAX.
    if(quantum==971 && quotient==((std::uint64_t(1)<<53)-1) && !remainder.Zero()) return false;
    UInt twice=remainder; if(!twice.Shift(1)) return false;
    const int half=twice.Compare(divisor);
    if(half>0 || (half==0 && (quotient&1u))) ++quotient;
    if(!quotient || quotient>(std::uint64_t(1)<<53)) return false;
    if(quotient==(std::uint64_t(1)<<53)) { quotient>>=1; ++quantum; }
    std::uint64_t bits=0;
    if(quotient>=(std::uint64_t(1)<<52)) {
        const int biased=quantum+52+1023;
        if(biased<=0 || biased>=2047) return false;
        bits=(static_cast<std::uint64_t>(biased)<<52)|(quotient-(std::uint64_t(1)<<52));
    } else {
        if(quantum!=trueMinExponent) return false;
        bits=quotient;
    }
    std::memcpy(&output,&bits,sizeof(output)); return true;
}
} } }
#endif
