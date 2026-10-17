// Standalone launcher-policy sanitizer gate; no solver arithmetic is exercised.
#include "../tests/FireRunHostMemory.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main()
{
    using namespace FireRunHostMemory;
    std::uint64_t random=1234u;
    const auto next=[&random](){random^=random<<13u;random^=random>>7u;random^=random<<17u;return random;};
    for(unsigned int iteration=0u;iteration<1000000u;++iteration){
        const Policy policy{next(),next()};
        const Sample sample{next(),next(),next(),(next()&1u)!=0u};
        // Addition in wider arithmetic is independent of Stop's subtraction.
        const bool expected=policy.reserve==0u||policy.ceiling<=policy.reserve||!sample.valid||
            static_cast<__uint128_t>(sample.rss)+policy.reserve>=policy.ceiling||
            static_cast<__uint128_t>(sample.footprint)+policy.reserve>=policy.ceiling;
        assert(policy.Stop(sample)==expected);
    }
    assert(!ProductionPolicy(std::numeric_limits<std::uint64_t>::max()).Valid());
#if defined(__APPLE__)
    const Sample live=Read();assert(live.valid&&live.rss>0u);
#endif
    std::puts("HOST_MEMORY_POLICY_SANITIZER randomized_cases=1000000 passed=1");
}
