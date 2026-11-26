#include "Agent/Json.h"
#include <cstdio>
#include <cstdlib>
#include <string>

// Embed the codec with a test-only destructor notification. The counter catches
// teardown even on platforms where freeing the special C locale is a no-op.
// No production debug API or per-parse accounting is introduced.
namespace {
unsigned localeDestructions=0;
unsigned lateCalls=0;
}
namespace RISE::Agent {
void JsonNumericLocaleDestroyedForTest() {++localeDestructions;}
}
#define RISE_JSON_NUMERIC_LOCALE_DESTRUCTION_TEST
#include "../src/Library/Agent/Json.cpp"

namespace {
void Fail(const char* reason) {
    std::fputs(reason,stderr);std::fputc('\n',stderr);std::fflush(stderr);
    std::_Exit(1);
}
void CheckLateCall() {
    // Check before parsing so the deliberately broken ownership mutant never
    // dereferences a destroyed object/freed locale during the RED control.
    if(localeDestructions!=0) Fail("numeric locale destroyed before late caller");
    RISE::Agent::JsonValue value;std::string error;
    if(!RISE::Agent::JsonParse("0.001",value,error)||!value.isNumber()||value.asNumber()!=.001)
        Fail("late numeric JSON parse failed");
    ++lateCalls;
}
void LateAtExit() {CheckLateCall();}
struct GlobalCaller {
    ~GlobalCaller() {
        CheckLateCall();
        if(lateCalls!=2) Fail("both late callers did not execute");
        std::fputs("AgentJsonLifetimeTest: PASS; atexit and global-destructor parses completed\n",stdout);
        std::fflush(stdout);
    }
};
GlobalCaller globalCaller;
}
int main() {
    // Register before first numeric parse: a function-static locale destructor
    // would be registered later, and therefore run before this callback.
    if(std::atexit(LateAtExit)!=0) Fail("atexit registration failed");
    RISE::Agent::JsonValue value;std::string error;
    if(!RISE::Agent::JsonParse("1.25",value,error)||value.asNumber()!=1.25)
        Fail("initial numeric JSON parse failed");
    return 0;
}
