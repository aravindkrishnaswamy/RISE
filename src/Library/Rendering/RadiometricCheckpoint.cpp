#include "pch.h"
#include "RadiometricCheckpoint.h"
#include "RadiometricPublication.h"
#include "../Utilities/DiskFileWriteBuffer.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace RISE { namespace Implementation {
namespace {
using RISECBOR64::Value;
using RISECBOR64::Bytes;
constexpr unsigned char magic[16]={'R','I','S','E','-','R','A','D','-','C','K','P','T','3','\r','\n'};
constexpr std::size_t prefixSize=24, digestSize=64, maxHeaderBytes=65536;
bool Error(std::string& error,const char* message) { error=message; return false; }
void Put64(Bytes& bytes,std::uint64_t value) { for(unsigned int i=0;i<8;++i) bytes.push_back(static_cast<unsigned char>(value>>(8u*i))); }
std::uint64_t Get64(const unsigned char* bytes) {
    std::uint64_t value=0;
    for(unsigned int i=0;i<8;++i) value|=static_cast<std::uint64_t>(bytes[i])<<(8u*i);
    return value;
}
void PutDouble(Bytes& bytes,double value) {
    static_assert(sizeof(double)==sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559,"checkpoint requires IEEE float64");
    std::uint64_t bits=0; std::memcpy(&bits,&value,sizeof(bits)); Put64(bytes,bits);
}
double GetDouble(const unsigned char* bytes) {
    const std::uint64_t bits=Get64(bytes); double value=0; std::memcpy(&value,&bits,sizeof(value)); return value;
}
const char* StatusName(RadiometricStatus status) {
    switch(status) {
    case RadiometricStatus::Running: return "running";
    case RadiometricStatus::Cancelled: return "cancelled";
    case RadiometricStatus::Complete: return "complete";
    case RadiometricStatus::Invalid: return "invalid";
    }
    return "unknown";
}
bool ExactKeys(const Value& value,const std::vector<std::string>& keys) {
    if(value.GetType()!=Value::Map || value.GetMap().size()!=keys.size()) return false;
    for(const auto& key:keys) if(!value.Find(key)) return false;
    return true;
}
bool CheckSize(std::size_t pixels,std::size_t header,std::size_t budget,std::size_t& total,std::string& error) {
    const auto fixed=prefixSize+digestSize;
    if(header>maxHeaderBytes || budget<fixed || header>budget-fixed || pixels>(budget-fixed-header)/48u)
        return Error(error,"radiometric checkpoint exceeds byte budget");
    total=fixed+header+pixels*48u;
    return true;
}
}
bool EncodeRadiometricCheckpoint(const RadiometricSnapshot& snapshot,Bytes& output,
    std::string& error,const RadiometricLimits& limits) {
    if(!ValidateRadiometricSnapshot(snapshot,error,limits)) return false;
    try {
        Bytes identity,header;
        if(!EncodeRadiometricIdentity(snapshot.identity,identity,error,limits)) return false;
        if(!RISECBOR64::Encode(Value::MapValue({
            {"record_kind",Value::String("radiometric_checkpoint_v3")},
            {"schema_version",Value::Unsigned(3)},
            {"identity",Value::BytesValue(identity)},
            {"identity_sha256",Value::String(RISECBOR64::SHA256Hex(identity))},
            {"pixel_count",Value::Unsigned(snapshot.pixels.size())},
            {"pixel_encoding",Value::String("le_u64_u64_f64_f64_f64_f64_v3")},
            {"status",Value::String(StatusName(snapshot.status))}
        }),header,&error)) return false;
        std::size_t total=0;
        if(!CheckSize(snapshot.pixels.size(),header.size(),limits.maxCheckpointBytes,total,error)) return false;
        Bytes bytes; bytes.reserve(total); bytes.insert(bytes.end(),magic,magic+sizeof(magic)); Put64(bytes,header.size());
        bytes.insert(bytes.end(),header.begin(),header.end());
        for(const auto& px:snapshot.pixels) { Put64(bytes,px.count); Put64(bytes,px.nextSampleIndex); PutDouble(bytes,px.mean); PutDouble(bytes,px.m2); PutDouble(bytes,px.meanCorrection); PutDouble(bytes,px.m2Correction); }
        const auto digest=RISECBOR64::SHA256Hex(bytes); bytes.insert(bytes.end(),digest.begin(),digest.end());
        output.swap(bytes);
        return true;
    } catch(const std::exception& ex) { error=ex.what(); return false; }
}
bool DecodeRadiometricCheckpoint(const Bytes& bytes,const RadiometricIdentity& expected,
    RadiometricSnapshot& output,std::string& error,const RadiometricLimits& limits) {
    error.clear();
    try {
        Bytes identity;
        if(!EncodeRadiometricIdentity(expected,identity,error,limits)) return false;
        if(bytes.size()<prefixSize+digestSize || bytes.size()>limits.maxCheckpointBytes ||
           !std::equal(magic,magic+sizeof(magic),bytes.begin())) return Error(error,"invalid radiometric checkpoint framing/byte budget");
        const auto headerLength=Get64(bytes.data()+16);
        if(headerLength>maxHeaderBytes || headerLength>bytes.size()-prefixSize-digestSize)
            return Error(error,"invalid radiometric checkpoint header length");
        const std::size_t bodyEnd=bytes.size()-digestSize;
        const std::string digest(bytes.begin()+bodyEnd,bytes.end());
        if(digest!=RISECBOR64::SHA256Hex(bytes.data(),bodyEnd)) return Error(error,"radiometric checkpoint SHA256 mismatch");
        Value header;
        if(!RISECBOR64::DecodeCanonical(bytes.data()+prefixSize,static_cast<std::size_t>(headerLength),header,&error)) return false;
        if(!ExactKeys(header,{"record_kind","schema_version","identity","identity_sha256","pixel_count","pixel_encoding","status"}))
            return Error(error,"radiometric checkpoint header key set mismatch");
        const auto* kind=header.Find("record_kind"); const auto* version=header.Find("schema_version");
        const auto* storedIdentity=header.Find("identity"); const auto* identityHash=header.Find("identity_sha256");
        const auto* count=header.Find("pixel_count"); const auto* encoding=header.Find("pixel_encoding"); const auto* status=header.Find("status");
        if(kind->GetType()!=Value::Text || kind->GetText()!="radiometric_checkpoint_v3" ||
           version->GetType()!=Value::UnsignedInteger || version->GetIntegerArgument()!=3 ||
           storedIdentity->GetType()!=Value::ByteString || storedIdentity->GetBytes()!=identity ||
           identityHash->GetType()!=Value::Text || identityHash->GetText()!=RISECBOR64::SHA256Hex(identity) ||
           count->GetType()!=Value::UnsignedInteger ||
           encoding->GetType()!=Value::Text || encoding->GetText()!="le_u64_u64_f64_f64_f64_f64_v3" || status->GetType()!=Value::Text)
            return Error(error,"radiometric checkpoint schema or resume identity mismatch");
        const auto expectedCount=static_cast<std::size_t>(expected.width)*expected.height;
        std::size_t total=0;
        if(count->GetIntegerArgument()!=expectedCount ||
           !CheckSize(expectedCount,static_cast<std::size_t>(headerLength),limits.maxCheckpointBytes,total,error) || total!=bytes.size())
            return Error(error,"radiometric checkpoint pixel length mismatch");
        RadiometricSnapshot result; result.identity=expected;
        if(status->GetText()=="running") result.status=RadiometricStatus::Running;
        else if(status->GetText()=="cancelled") result.status=RadiometricStatus::Cancelled;
        else if(status->GetText()=="complete") result.status=RadiometricStatus::Complete;
        else return Error(error,"radiometric checkpoint is invalid or has unknown status");
        result.pixels.resize(expectedCount);
        const unsigned char* cursor=bytes.data()+prefixSize+static_cast<std::size_t>(headerLength);
        for(auto& px:result.pixels) { px.count=Get64(cursor); px.nextSampleIndex=Get64(cursor+8); px.mean=GetDouble(cursor+16); px.m2=GetDouble(cursor+24); px.meanCorrection=GetDouble(cursor+32); px.m2Correction=GetDouble(cursor+40); cursor+=48; }
        if(!ValidateRadiometricSnapshot(result,error,limits)) return false;
        output=std::move(result);
        return true;
    } catch(const std::exception& ex) { error=ex.what(); return false; }
}
bool RadiometricDetail::SaveCheckpointObserved(const std::string& path,const RadiometricSnapshot& snapshot,
    std::string& error,const RadiometricLimits& limits,const std::function<void()>& beforePublication) {
    Bytes bytes;
    if(!EncodeRadiometricCheckpoint(snapshot,bytes,error,limits)) return false;
    DiskFileWriteBuffer::WriteDiagnostics diagnostics;
    const bool success=DiskFileWriteBuffer::WriteAtomically(path,[&](DiskFileWriteBuffer& file) {
        std::size_t offset=0;
        while(offset<bytes.size()) {
            const auto count=static_cast<unsigned int>(std::min<std::size_t>(bytes.size()-offset,std::numeric_limits<unsigned int>::max()));
            if(!file.setBytes(bytes.data()+offset,count)) return false;
            offset+=count;
        }
        if(beforePublication) beforePublication();
        return true;
    },DiskFileWriteBuffer::StagingPolicy::Required,&diagnostics);
    if(!success) {
        error="radiometric checkpoint publication failed: "+diagnostics.operation+": "+diagnostics.error.message();
        if(!diagnostics.recoveryPath.empty())
            error+="; completed output retained for recovery at '"+diagnostics.recoveryPath+"'";
    }
    return success;
}
bool SaveRadiometricCheckpoint(const std::string& path,const RadiometricSnapshot& snapshot,
    std::string& error,const RadiometricLimits& limits) {
    return RadiometricDetail::SaveCheckpointObserved(path,snapshot,error,limits,{});
}
bool LoadRadiometricCheckpoint(const std::string& path,const RadiometricIdentity& expected,
    RadiometricSnapshot& output,std::string& error,const RadiometricLimits& limits) {
    try {
        // Read one opened file; identity cannot change through a path reopen.
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if(!input) return Error(error,"cannot open radiometric checkpoint");
        const auto end=input.tellg();
        if(end<0 || static_cast<std::uint64_t>(end)>limits.maxCheckpointBytes ||
           static_cast<std::uint64_t>(end)>static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            return Error(error,"radiometric checkpoint exceeds read byte budget");
        Bytes bytes(static_cast<std::size_t>(end)); input.seekg(0);
        if(!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
            return Error(error,"cannot read radiometric checkpoint");
        if(input.peek()!=std::char_traits<char>::eof()) return Error(error,"radiometric checkpoint changed size during read");
        return DecodeRadiometricCheckpoint(bytes,expected,output,error,limits);
    } catch(const std::exception& ex) { error=ex.what(); return false; }
}
} }
