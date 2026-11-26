#ifndef RISE_TEST_FIRE_RUN_HOST_MEMORY_H
#define RISE_TEST_FIRE_RUN_HOST_MEMORY_H

#include <algorithm>
#include <cstdint>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

// Launcher resource policy, not a numerical acceptance tolerance. RSS and
// footprint are separate quantities: compression must not hide retention.
namespace FireRunHostMemory
{
    constexpr int GracefulStopExitCode=97;
    struct Sample {
        std::uint64_t rss=0u,footprint=0u,compressed=0u;
        bool valid=false;
    };
    inline Sample Read()
    {
        Sample sample;
#if defined(__APPLE__)
        task_vm_info_data_t info={};
        mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
        if(task_info(mach_task_self(),TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info),&count)==KERN_SUCCESS&&
            count>=TASK_VM_INFO_REV1_COUNT){
            sample.rss=info.resident_size;sample.footprint=info.phys_footprint;
            sample.compressed=info.compressed;sample.valid=sample.rss>0u;
        }
#elif defined(_WIN32)
        // M8b, not compiled here: working set as RSS, private commit as the footprint.
        PROCESS_MEMORY_COUNTERS_EX counters={};counters.cb=sizeof(counters);
        if(K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))){
            sample.rss=counters.WorkingSetSize;sample.footprint=counters.PrivateUsage;
            sample.valid=sample.rss>0u;
        }
#elif defined(__linux__)
        // M8b, not compiled here: resident pages from /proc/self/statm (Linux, Android).
        if(std::FILE* statm=std::fopen("/proc/self/statm","r")){
            unsigned long long size=0u,resident=0u;
            const long page=sysconf(_SC_PAGESIZE);
            if(std::fscanf(statm,"%llu %llu",&size,&resident)==2&&page>0){
                sample.rss=static_cast<std::uint64_t>(resident)*static_cast<std::uint64_t>(page);
                sample.footprint=sample.rss;sample.valid=sample.rss>0u;}
            std::fclose(statm);
        }
#endif
        return sample;
    }
    //! M8b: physical memory of the host in bytes; zero when unknown.
    inline std::uint64_t PhysicalBytes()
    {
#if defined(__APPLE__)
        std::uint64_t physical=0u;std::size_t size=sizeof(physical);
        if(sysctlbyname("hw.memsize",&physical,&size,nullptr,0)!=0||size!=sizeof(physical))return 0u;
        return physical;
#elif defined(_WIN32)
        MEMORYSTATUSEX status={};status.dwLength=sizeof(status);
        return GlobalMemoryStatusEx(&status)?static_cast<std::uint64_t>(status.ullTotalPhys):0u;
#elif defined(__linux__)
        const long pages=sysconf(_SC_PHYS_PAGES),page=sysconf(_SC_PAGESIZE);
        return pages>0&&page>0?static_cast<std::uint64_t>(pages)*static_cast<std::uint64_t>(page):0u;
#else
        return 0u;
#endif
    }
    struct Policy {
        std::uint64_t ceiling=0u,reserve=0u;
        bool Valid() const {return ceiling>reserve&&reserve>0u;}
        bool Stop(const Sample& sample) const {
            return !Valid()||!sample.valid||
                std::max(sample.rss,sample.footprint)>=ceiling-reserve;
        }
    };
    inline Policy ProductionPolicy(const std::uint64_t ownerCertificate)
    {
        Policy policy;
        // M8b: the same policy on every host whose memory size is known (it was
        // macOS-only while fire needed Metal).
        const std::uint64_t physical=PhysicalBytes();
        if(physical==0u)return policy;
        // At most half of host RAM; 32 GiB caps this single worker even
        // on larger hosts. Reserve the complete next owner peak plus 2 GiB
        // for host scratch/checkpoint publication. These are resource budgets.
        policy.ceiling=std::min(physical/2u,UINT64_C(32)<<30u);
        if(ownerCertificate>UINT64_MAX-(UINT64_C(2)<<30u))return Policy();
        policy.reserve=ownerCertificate+(UINT64_C(2)<<30u);
        return policy;
    }
}
#endif
