// Standalone red/green driver for BDPTDepthCapMISTest row R.
#define main BDPTDepthCapOriginalDriver
#include "BDPTDepthCapMISTest.cpp"
#undef main
int main() { CoatedSingleThreadOptions(); RowR(); std::printf("%d passed, %d failed\n",passCount,failCount); return failCount ? 1 : 0; }
