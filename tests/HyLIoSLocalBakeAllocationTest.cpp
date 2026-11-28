// Standalone allocation regression. The observer in the included test is active
// only during single-threaded metadata rejection calls, with fixtures and labels
// constructed first. It observes ordinary new/new[] requests and is not a global
// thread-safe allocator profiler. ASan/UBSan builds use these same replacements;
// no sanitizer options or allocation-mismatch suppressions are applied.
#define HYLIOS_ALLOCATION_GATE 1
#include "HyLIoSLocalBakeTest.cpp"
