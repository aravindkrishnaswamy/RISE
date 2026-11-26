// PRIVATE publication implementation/test seam, not a renderer setting/API.
// License: LICENSE.TXT.
#ifndef RISE_RADIOMETRIC_PUBLICATION_DETAIL_H
#define RISE_RADIOMETRIC_PUBLICATION_DETAIL_H
#include "RadiometricFilm.h"
#include <functional>
namespace RISE { namespace Implementation { namespace RadiometricDetail {
// Public wrappers call these with no observer. Tests may observe the boundary
// after encoding and before the SAME atomic helper's Finish/rename operations.
// Per-call only: no global hooks, scene state, TLS, or alternative writer path.
bool SaveCheckpointObserved(const std::string&,const RadiometricSnapshot&,std::string&,
    const RadiometricLimits&,const std::function<void()>& beforePublication);
bool WriteEXRObserved(const std::string&,const RadiometricSnapshot&,std::string&,
    const RadiometricLimits&,const std::function<void()>& beforePublication);
} } }
#endif
