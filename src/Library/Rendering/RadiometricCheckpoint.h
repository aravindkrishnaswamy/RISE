// Fixed-wavelength raw-film checkpoint codec. License: LICENSE.TXT.
#ifndef RISE_RADIOMETRIC_CHECKPOINT_H
#define RISE_RADIOMETRIC_CHECKPOINT_H
#include "RadiometricFilm.h"
namespace RISE { namespace Implementation {
// Pixel state is 48-byte little-endian records (count,index,mean,m2,meanCorrection,m2Correction); double
// fields retain exact IEEE-754 bits. Header is bounded canonical RISE-CBOR64.
// Digest detects corruption, not malicious re-signing or source authenticity.
bool EncodeRadiometricCheckpoint(const RadiometricSnapshot&, RISECBOR64::Bytes&,
    std::string& error, const RadiometricLimits& = {});
bool DecodeRadiometricCheckpoint(const RISECBOR64::Bytes&, const RadiometricIdentity& expected,
    RadiometricSnapshot& output, std::string& error, const RadiometricLimits& = {});
// On late publication failure, the diagnostic names any completed staging
// output deliberately retained by the shared atomic helper for recovery.
bool SaveRadiometricCheckpoint(const std::string& path, const RadiometricSnapshot&,
    std::string& error, const RadiometricLimits& = {});
bool LoadRadiometricCheckpoint(const std::string& path, const RadiometricIdentity& expected,
    RadiometricSnapshot& output, std::string& error, const RadiometricLimits& = {});
} }
#endif
