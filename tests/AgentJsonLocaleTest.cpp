#include "Agent/Json.h"
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>
using J=RISE::Agent::JsonValue;
namespace {
class CommaPunctuation final : public std::numpunct<char> {
protected:
    char do_decimal_point() const override {return ',';}
};
void Check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
double Parse(const std::string& token) {
    J value;std::string error;
    Check(RISE::Agent::JsonParse(token,value,error)&&value.isNumber(),"numeric parse failed");
    return value.asNumber();
}
std::string LegacyFormatting(double value) {
    char buffer[128];
    if(value==std::floor(value)&&std::fabs(value)<9e15)
        std::snprintf(buffer,sizeof(buffer),"%lld",static_cast<long long>(value));
    else std::snprintf(buffer,sizeof(buffer),"%.17g",value);
    return buffer;
}
}
int main(int argc,char**) {
    const char* localeBefore=std::setlocale(LC_NUMERIC,nullptr);
    const std::string saved=localeBefore ? localeBefore : "C";
    const std::locale savedCppLocale;
    try {
        Check(std::setlocale(LC_NUMERIC,"C")!=nullptr,"C locale unavailable");
        std::vector<double> values={0.,-0.,.001,-.001,1.,1e20,1e-20,9007199254740992.,
            std::numeric_limits<double>::min(),std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max()};
        std::uint64_t state=0x12345678;
        for(unsigned i=0;i<10000;++i) {
            state=state*6364136223846793005ULL+1442695040888963407ULL;
            double value=0;std::memcpy(&value,&state,sizeof(value));if(std::isfinite(value)) values.push_back(value);
        }
        std::vector<std::string> expected;
        for(double value:values) {
            expected.push_back(LegacyFormatting(value));
            Check(RISE::Agent::JsonSerialize(J::MakeNumber(value))==expected.back(),"C locale formatting changed");
        }
        // A host application's C++ locale is separate from its C numeric locale.
        std::locale::global(std::locale(std::locale::classic(),new CommaPunctuation));
        Check(RISE::Agent::JsonSerialize(J::MakeNumber(.001))==expected[2],"C++ locale affected serialization");
        unsigned commaLocales=0;
        for(const char* locale:{"C","de_DE.UTF-8","fr_FR.UTF-8","de_DE.utf8","German_Germany.1252"}) {
            const char* active=std::setlocale(LC_NUMERIC,locale);if(!active) continue;
            const std::string activeName=active;
            if(std::string(std::localeconv()->decimal_point)==",") ++commaLocales;
            Check(Parse("0.001")==.001&&Parse("-1.25e-3")==-.00125,"decimal locale affected parse");
            Check(Parse("5e-324")==std::numeric_limits<double>::denorm_min(),"subnormal parse changed");
            Check(Parse("1e-9999")==0&&std::isinf(Parse("1e9999")),"range conversion semantics changed");
            for(std::size_t i=0;i<values.size();++i) {
                const auto text=RISE::Agent::JsonSerialize(J::MakeNumber(values[i]));
                Check(text==expected[i],"numeric locale affected serialization");
                Check(Parse(text)==values[i],"binary64 roundtrip differs");
            }
            for(const char* malformed:{"01","-01","1.","1e","1e+","-","+1",".1","0,1","1e-"}) {
                J parsed;std::string error;Check(!RISE::Agent::JsonParse(malformed,parsed,error),"malformed numeric grammar accepted");
            }
            Check(activeName==std::setlocale(LC_NUMERIC,nullptr),"codec mutated process locale");
        }
        Check(argc==1||commaLocales>0,"required comma locale unavailable");
        std::locale::global(savedCppLocale);
        std::setlocale(LC_NUMERIC,saved.c_str());
        std::cout<<"AgentJsonLocaleTest: PASS; comma locales exercised="<<commaLocales<<'\n';
    } catch(const std::exception& error) {
        std::locale::global(savedCppLocale);
        std::setlocale(LC_NUMERIC,saved.c_str());std::cerr<<error.what()<<'\n';return 1;
    }
}
