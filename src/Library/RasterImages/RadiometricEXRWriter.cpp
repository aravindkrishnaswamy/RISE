#include "pch.h"
#include "RadiometricEXRWriter.h"
#include "../Rendering/RadiometricPublication.h"
#include "EXRWriter.h"
#include "../Utilities/DiskFileWriteBuffer.h"
#include <limits>
#include <cmath>
#include <cstring>
#ifndef NO_EXR_SUPPORT
#include <ImfDoubleAttribute.h>
#include <ImfStringAttribute.h>
#endif
namespace RISE { namespace Implementation {
namespace {
bool FloatIsZero(float value) {
    volatile float barrier=value;
    const float materialized=barrier;
    std::uint32_t bits=0;
    std::memcpy(&bits,&materialized,sizeof(bits));
    return (bits & 0x7fffffffu)==0;
}
bool FloatCastIsZero(double value) { return FloatIsZero(static_cast<float>(value)); }
bool PositiveCorrection(double value) {
    volatile double barrier=value;
    const double materialized=barrier;
    std::uint64_t bits=0; std::memcpy(&bits,&materialized,sizeof(bits));
    return (bits>>63)==0;
}
float ResolveOrdinate(const RadiometricPixel& pixel) {
    volatile float initial=static_cast<float>(pixel.mean);
    const float value=initial;
    if(IsZeroDouble(pixel.meanCorrection)) return value;
    const bool positive=PositiveCorrection(pixel.meanCorrection);
    const float neighbor=std::nextafter(value,positive ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
    if(!IsFiniteDouble(static_cast<double>(neighbor))) return value;
    // A normalized binary64 residual can change float32 rounding only when
    // the high part lies EXACTLY on a float32 midpoint. Midpoints are exactly
    // representable in binary64, including half the smallest FLOAT subnormal.
    volatile double midpoint=(static_cast<double>(value)+static_cast<double>(neighbor))*0.5;
    return pixel.mean==midpoint ? neighbor : value;
}
}
bool RadiometricDetail::WriteEXRObserved(const std::string& path,const RadiometricSnapshot& snapshot,
    std::string& error,const RadiometricLimits& limits,const std::function<void()>&
#ifndef NO_EXR_SUPPORT
    beforePublication
#endif
    ) {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559,"raw EXR requires IEEE float32");
    error.clear();
    if(!ValidateRadiometricSnapshot(snapshot,error,limits)) return false;
    if(snapshot.status!=RadiometricStatus::Complete) { error="radiometric EXR requires a complete measurement"; return false; }
    if(snapshot.identity.width>static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
       snapshot.identity.height>static_cast<unsigned int>(std::numeric_limits<int>::max())) {
        error="radiometric dimensions exceed OpenEXR integer range"; return false;
    }
    if(snapshot.identity.pixelAspectRatio>static_cast<double>(std::numeric_limits<float>::max()) ||
       FloatCastIsZero(snapshot.identity.pixelAspectRatio)) {
        error="radiometric pixel aspect ratio is not representable as OpenEXR FLOAT"; return false;
    }
    for(const auto& px:snapshot.pixels) {
        const double maximum=static_cast<double>(std::numeric_limits<float>::max());
        if(px.mean>maximum || (px.mean==maximum && !IsZeroDouble(px.meanCorrection) && PositiveCorrection(px.meanCorrection))) {
            error="radiometric ordinate exceeds FLOAT output range"; return false;
        }
        if(!IsZeroDouble(px.mean) && FloatIsZero(ResolveOrdinate(px))) {
            error="radiometric positive ordinate underflows FLOAT output"; return false;
        }
    }
#ifdef NO_EXR_SUPPORT
    error="raw radiometric EXR output unavailable: NO_EXR_SUPPORT"; return false;
#else
    RISECBOR64::Bytes identity;
    if(!EncodeRadiometricIdentity(snapshot.identity,identity,error,limits)) return false;
    DiskFileWriteBuffer::WriteDiagnostics diagnostics;
    const bool success=DiskFileWriteBuffer::WriteAtomically(path,[&](DiskFileWriteBuffer& file) {
        try {
            OStreamWrapper stream(file,BufferPosition(file));
            Imf::Header header(static_cast<int>(snapshot.identity.width),static_cast<int>(snapshot.identity.height));
            header.compression()=Imf::ZIP_COMPRESSION;
            header.pixelAspectRatio()=static_cast<float>(snapshot.identity.pixelAspectRatio);
            header.insert("rise.pixel_aspect_ratio",Imf::DoubleAttribute(snapshot.identity.pixelAspectRatio));
            header.channels().insert("L_lambda",Imf::Channel(Imf::FLOAT));
            header.insert("rise.radiometric_record",Imf::StringAttribute("radiometric_fixed_wavelength_plane_v3"));
            header.insert("rise.accumulator",Imf::StringAttribute("compensated_welford_scaled_delta_square_v3"));
            header.insert("rise.observable",Imf::StringAttribute("spectral_radiance_density"));
            header.insert("rise.units",Imf::StringAttribute("W m^-2 sr^-1 nm^-1"));
            header.insert("rise.wavelength_measure",Imf::StringAttribute("fixed_ordinate_no_bin_factor"));
            header.insert("rise.wavelength_nm",Imf::DoubleAttribute(snapshot.identity.wavelengthNM));
            header.insert("rise.identity_sha256",Imf::StringAttribute(RISECBOR64::SHA256Hex(identity)));
            header.insert("rise.provenance_sha256",Imf::StringAttribute(RISECBOR64::SHA256Hex(snapshot.identity.provenance)));
            header.insert("rise.target_samples",Imf::StringAttribute(std::to_string(snapshot.identity.targetSamples)));
            header.insert("rise.seed",Imf::StringAttribute(std::to_string(snapshot.identity.seed)));
            header.insert("rise.seed_algorithm",Imf::StringAttribute(snapshot.identity.seedAlgorithm));
            std::vector<float> row(snapshot.identity.width);
            Imf::FrameBuffer buffer;
            buffer.insert("L_lambda",Imf::Slice(Imf::FLOAT,reinterpret_cast<char*>(row.data()),sizeof(float),0));
            Imf::OutputFile output(stream,header);
            output.setFrameBuffer(buffer);
            for(std::uint32_t y=0;y<snapshot.identity.height;++y) {
                for(std::uint32_t x=0;x<snapshot.identity.width;++x)
                    row[x]=ResolveOrdinate(snapshot.pixels[static_cast<std::size_t>(y)*snapshot.identity.width+x]);
                output.writePixels(1);
            }
            if(stream.Failed()) return false;
            if(beforePublication) beforePublication();
            return true;
        } catch(const std::exception& ex) { error=ex.what(); return false; }
    },DiskFileWriteBuffer::StagingPolicy::Required,&diagnostics);
    if(!success) {
        if(error.empty()) error="radiometric EXR publication failed: "+diagnostics.operation+": "+diagnostics.error.message();
        if(!diagnostics.recoveryPath.empty())
            error+="; completed output retained for recovery at '"+diagnostics.recoveryPath+"'";
    }
    return success;
#endif
}
bool WriteRadiometricEXR(const std::string& path,const RadiometricSnapshot& snapshot,
    std::string& error,const RadiometricLimits& limits) {
    return RadiometricDetail::WriteEXRObserved(path,snapshot,error,limits,{});
}
} }
