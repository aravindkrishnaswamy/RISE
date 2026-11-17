// DL-500: an observable allocation budget on the production extended PT path.
// Reuse the fixed ball-lens fixture; counting is absent from timing builds.
#define RISE_DL500_COUNT_NEW
#define main SMSExtendedPartitionEntry
#include "SMSExtendedPartitionTest.cpp"
#undef main
int main() {
    const char* args[]={"SMSExtendedAllocationTest","--section","dl500-sample",
        "--cost-scene","DL372","--allocation-limit","68"};
    char* argv[7];
    for(unsigned i=0;i<7;++i) argv[i]=const_cast<char*>(args[i]);
    return SMSExtendedPartitionEntry(7,argv);
}
