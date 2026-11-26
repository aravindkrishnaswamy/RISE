// Reciprocal two-sided Kulla-Conty compensation. The energy table is a
// numerical approximation of the unmodified height-correlated GGX kernel.
#ifndef RISE_ROUGH_DIELECTRIC_ENERGY_H
#define RISE_ROUGH_DIELECTRIC_ENERGY_H
#include "MicrofacetUtils.h"
#include <array>

namespace RISE { namespace RoughDielectricEnergy {
inline constexpr int RoughCount=10, RatioCount=6, IndexCount=38, PhiCount=8, CosCount=41;
inline constexpr Scalar RoughNodes[RoughCount]={0,0.001,0.0030000000000000001,0.01,0.029999999999999999,0.080000000000000002,0.20000000000000001,0.40000000000000002,0.69999999999999996,1};
inline constexpr Scalar RatioNodes[RatioCount]={0,.04,.15,.4,.7,1};
// Bounded index contrast |eta-1|/(eta+1); endpoints include equal-index limits
// and the conductor limit. Exact eta==1 remains a straight-through delta.
inline constexpr Scalar IndexNodes[IndexCount]={0,5.5511151231257827e-17,2.2204460492503131e-16,8.8817841970012523e-16,3.5527136788005009e-15,1.4210854715202004e-14,5.6843418860808015e-14,2.2737367544323206e-13,9.0949470177292824e-13,3.637978807091713e-12,1.4551915228366852e-11,5.8207660913467407e-11,2.3283064365386963e-10,9.3132257461547852e-10,3.7252902984619141e-09,1.4901161193847656e-08,5.9604644775390625e-08,2.384185791015625e-07,9.5367431640625e-07,3.814697265625e-06,1.52587890625e-05,6.103515625e-05,0.000244140625,0.0009765625,0.0050000000000000001,0.02,0.040000000000000001,0.070000000000000007,0.10000000000000001,0.14999999999999999,0.20000000000000001,0.25,0.34999999999999998,0.5,0.65000000000000002,0.80000000000000004,0.94999999999999996,1};
inline constexpr Scalar CosNodes[CosCount]={0,5.5511151231257827e-17,2.2204460492503131e-16,8.8817841970012523e-16,3.5527136788005009e-15,1.4210854715202004e-14,5.6843418860808015e-14,2.2737367544323206e-13,9.0949470177292824e-13,3.637978807091713e-12,1.4551915228366852e-11,5.8207660913467407e-11,2.3283064365386963e-10,9.3132257461547852e-10,3.7252902984619141e-09,1.4901161193847656e-08,5.9604644775390625e-08,2.384185791015625e-07,9.5367431640625e-07,3.814697265625e-06,1.52587890625e-05,6.103515625e-05,0.000244140625,0.0009765625,0.0040000000000000001,0.01,0.02,0.040000000000000001,0.070000000000000007,0.10000000000000001,0.14999999999999999,0.20000000000000001,0.29999999999999999,0.40000000000000002,0.5,0.59999999999999998,0.69999999999999996,0.80000000000000004,0.90000000000000002,0.94999999999999996,1};
inline Scalar CosNode(int i) { return CosNodes[i]; }
inline constexpr int RowCount=RoughCount*RatioCount*IndexCount*2;
inline constexpr int RowSize=PhiCount*CosCount;

// Directions are unit vectors in the shading frame; incident z is positive.
class Model {
    Vector3 wi;
    Scalar ax,ay,eta;
    bool swapAxes;
    int side;
    std::array<int,8> rows;
    std::array<Scalar,8> weights;
    Scalar means[2];
    Scalar missing, tailTransmission, proposal;
    Scalar Node(int face,int phi,int cosine) const;
    Scalar Missing(const Vector3& direction,int face) const;
    Vector3 DrawTail(int face,Scalar uPhi,Scalar uCos) const;
    MicrofacetUtils::RoughDielectric::PositiveProduct TailPdf(const Vector3& wo) const;
    MicrofacetUtils::RoughDielectric::Evaluation Combine(const Vector3& wo,
        MicrofacetUtils::RoughDielectric::Evaluation ss,bool radiance) const;
public:
    Model(const Vector3& incident,Scalar alphaX,Scalar alphaY,Scalar relativeIOR);
    Scalar MissingEnergy() const { return missing; }
    Scalar MeanMissing(int opposite=0) const { return means[side ^ opposite]; }
    MicrofacetUtils::RoughDielectric::Evaluation Evaluate(const Vector3& wo,bool radiance) const;
    MicrofacetUtils::RoughDielectric::Sample Draw(Scalar u,Scalar v,Scalar lobe,Scalar technique,bool radiance) const;
};
} }
#endif
