// Import worker for RISE simulation cache playback; never loaded by the renderer.
#include "../src/Library/Utilities/FireSequenceCache.h"
#include "SimulationVDBImport.h"
#include <algorithm>
#include <charconv>
#include <exception>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__unix__)
#include <sys/resource.h>
#endif

namespace {
bool SetImportMemoryBudget()
{
#ifdef __APPLE__
    // RLIMIT_AS is unsupported on macOS. Decoder bounds are the primary
    // control; terminate this isolated process if its observed footprint
    // exceeds half the caller's 32 GiB ceiling, leaving headroom for the
    // next decode allocation and for the renderer. Measure resident pages
    // as well as footprint (shared mapped memory is accounted differently).
    auto check = [] {
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        const auto result = task_info(mach_task_self(), TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info), &count);
        return result == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT &&
            info.phys_footprint < (16ull << 30) && info.resident_size < (16ull << 30);
    };
    if (!check()) return false;
    std::thread([check] {
        for (;;) {
            if (!check()) {
                std::fputs("simulation import exceeded memory budget\n",stderr);
                std::_Exit(75);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
    return true;
#elif defined(RLIMIT_AS)
    struct rlimit limit{};
    if (getrlimit(RLIMIT_AS,&limit) != 0) return false;
    limit.rlim_cur = std::min<rlim_t>(limit.rlim_cur,16ull << 30);
    return setrlimit(RLIMIT_AS,&limit) == 0;
#else
    return false;
#endif
}
}

int main(int argc, char** argv)
{
    try {
        if (!SetImportMemoryBudget()) {
            std::fputs("unable to enforce or monitor import memory budget\n",stderr);
            return 75;
        }
        if (argc >= 2 && !std::strcmp(argv[1],"--vdb"))
            return ImportForeignVDB(argc,argv);
        if (argc != 6 || std::strcmp(argv[1],"legacy")) {
            std::fputs("usage: rise-cache-import legacy SOURCE CACHE ENVELOPE_SHA256 ORDINAL|all\n"
                       "       rise-cache-import --vdb ...\n",stderr);
            return 64;
        }
        const std::string digest(argv[4]);
        if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos) {
            std::fputs("invalid envelope SHA-256\n",stderr);
            return 64;
        }
        std::optional<std::int64_t> ordinal;
        if (std::strcmp(argv[5],"all")) {
            std::int64_t value{};
            const char* end = argv[5] + std::strlen(argv[5]);
            const auto parsed = std::from_chars(argv[5],end,value);
            if (parsed.ec != std::errc{} || parsed.ptr != end) {
                std::fputs("invalid frame ordinal\n",stderr);
                return 64;
            }
            ordinal = value;
        }
        std::string error, generation;
        if (!RISE::Implementation::ImportFireSequenceCache(argv[2],argv[3],error,ordinal,digest,&generation)) {
            std::fprintf(stderr,"simulation import: %s\n",error.c_str());
            return 1;
        }
        std::puts(generation.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"simulation import: %s\n",e.what());
        return 1;
    }
}
