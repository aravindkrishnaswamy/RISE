// DL-500: an observable allocation budget on the production extended PT path.
// Reuse the fixed ball-lens fixture; counting is absent from timing builds.
#define RISE_DL500_COUNT_NEW
#define main SMSExtendedPartitionEntry
#include "SMSExtendedPartitionTest.cpp"
#undef main
int main() {
    const char* args[]={"SMSExtendedAllocationTest","--section","dl500-sample",
        "--cost-scene","DL372","--cost-spp","16","--allocation-limit","68"};
    constexpr unsigned argc=sizeof(args)/sizeof(args[0]);
    char* argv[argc];
    for(unsigned i=0;i<argc;++i) argv[i]=const_cast<char*>(args[i]);
    return SMSExtendedPartitionEntry(int(argc),argv);
}
