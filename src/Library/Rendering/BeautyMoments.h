// Compensated signed luminance moments for beauty adaptive sampling.
// License Information: Please see the attached LICENSE.TXT file.
#ifndef RISE_BEAUTY_MOMENTS_H
#define RISE_BEAUTY_MOMENTS_H
#include <cstdint>

namespace RISE { namespace Implementation {
// Persist this entire state between progressive passes. Corrections are part
// of the estimator; reconstructing from rounded mean/M2 loses small updates.
struct BeautyMoments
{
    double mean = 0, meanCorrection = 0;
    double m2 = 0, m2Correction = 0;
    std::uint32_t count = 0;
    bool valid = true;

    // Every trial, including misses (zero), counts. A numeric range failure is
    // sticky and disables adaptive termination without altering beauty color.
    void Add(double luminance);
    bool StandardError(double& result) const;
    bool IsConverged(double threshold, double nearZero) const;
};
} }
#endif
