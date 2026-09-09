#ifndef RISE_TEST_FIRE_RUN_HOST_MEMORY_H
#define RISE_TEST_FIRE_RUN_HOST_MEMORY_H

#include <algorithm>
#include <cstdint>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
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
#endif
        return sample;
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
#if defined(__APPLE__)
        std::uint64_t physical=0u;std::size_t size=sizeof(physical);
        if(sysctlbyname("hw.memsize",&physical,&size,nullptr,0)!=0||
            size!=sizeof(physical))return policy;
        // At most half of host RAM; 32 GiB caps this single worker even
        // on larger hosts. Reserve the complete next owner peak plus 2 GiB
        // for host scratch/checkpoint publication. These are resource budgets.
        policy.ceiling=std::min(physical/2u,UINT64_C(32)<<30u);
        if(ownerCertificate>UINT64_MAX-(UINT64_C(2)<<30u))return Policy();
        policy.reserve=ownerCertificate+(UINT64_C(2)<<30u);
#else
        (void)ownerCertificate;
#endif
        return policy;
    }
}
#endif
