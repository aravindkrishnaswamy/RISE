// DL-536: copies own their cursor; std::mt19937 is the independent oracle.
#include "../src/Library/Utilities/MersenneTwister.h"
#include <random>
#include <iostream>
#include <memory>
#include <string>
static unsigned passed=0,failed=0;
static void Check(bool ok,const char* name){if(ok)++passed;else{++failed;std::cout<<"FAIL: "<<name<<"\n";}}
static void CopyCase(unsigned warm,bool assignment) {
 RISE::MersenneTwister source(536);std::mt19937 ref(536);
 bool prefix=true;for(unsigned i=0;i<warm;++i)prefix &= source.genrand_int32()==ref();
 Check(prefix,"uncopied prefix agrees with standard MT19937");
 auto copy=assignment?std::make_unique<RISE::MersenneTwister>(999):std::make_unique<RISE::MersenneTwister>(source);
 if(assignment){for(unsigned i=0;i<37;++i)copy->genrand_int32();*copy=source;}
 for(unsigned i=0;i<4096;++i)source.genrand_int32();
 unsigned wrong=0;for(unsigned i=0;i<1280;++i)if(copy->genrand_int32()!=ref())++wrong;
 std::cout<<"DL536 warm="<<warm<<" assignment="<<assignment<<" mismatches="<<wrong<<"/1280 (n=1 sd=0)\n";
 Check(wrong==0,"snapshot does not read the advancing source state");
}
static void Lifetime() {
 auto source=std::make_unique<RISE::MersenneTwister>(536);std::mt19937 ref(536);
 for(unsigned i=0;i<13;++i){source->genrand_int32();ref();}
 RISE::MersenneTwister copy(*source);source.reset();
 bool match=true;for(unsigned i=0;i<4096;++i)match &= copy.genrand_int32()==ref();
 Check(match,"copied stream outlives its source");
}
int main(int argc,char** argv) {
 for(unsigned warm:{0u,13u,623u,624u,625u})for(bool assign:{false,true})CopyCase(warm,assign);
 RISE::MersenneTwister unseeded;RISE::MersenneTwister a(unseeded),b(999);b=unseeded;
 std::mt19937 ref;bool defaults=true;for(unsigned i=0;i<4096;++i){const auto x=ref();defaults &= a.genrand_int32()==x && b.genrand_int32()==x;}
 Check(defaults,"uninitialized-generator copies retain the default seed semantics");
 RISE::MersenneTwister self(536);std::mt19937 selfRef(536);for(unsigned i=0;i<13;++i){self.genrand_int32();selfRef();}
 auto* alias=&self;self=*alias;bool unchanged=true;for(unsigned i=0;i<4096;++i)unchanged &= self.genrand_int32()==selfRef();
 Check(unchanged,"self assignment preserves the sequence");
 if(argc==2&&std::string(argv[1])=="--lifetime")Lifetime();
 std::cout<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
