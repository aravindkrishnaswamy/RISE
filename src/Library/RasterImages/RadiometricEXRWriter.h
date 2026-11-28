// Scientific scalar EXR output, independent of RGB/alpha/view transforms.
// License: LICENSE.TXT.
#ifndef RISE_RADIOMETRIC_EXR_WRITER_H
#define RISE_RADIOMETRIC_EXR_WRITER_H
#include "../Rendering/RadiometricFilm.h"
namespace RISE { namespace Implementation {
// Writes a COMPLETE fixed-wavelength measurement as a lossless single FLOAT
// L_lambda channel. Refuses invalid, overflowed or positive-to-zero narrowed
// values; no sanitization/clamp/color conversion/premultiplication.
// Uses required atomic sibling staging; callers serialize writes to aliases.
// Late publication failure diagnostics name any completed output deliberately
// retained by the shared atomic helper for recovery.
bool WriteRadiometricEXR(const std::string& path,const RadiometricSnapshot&,
    std::string& error,const RadiometricLimits& = {});
} }
#endif
