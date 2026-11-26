// M8b: the M8 exit criterion as its own fast-tier test (each test stays near 20 s):
// CPU fast mode against Metal fast mode on the coarse case, step by step, skipped
// where Metal is absent.  Same source as FireCPUBackendTest (see its header).
#define RISE_FIRE_CPU_AGREEMENT_ONLY 1
#include "FireCPUBackendTest.cpp"
