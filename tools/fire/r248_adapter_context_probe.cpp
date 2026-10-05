#include <array>
#include <cstring>
namespace RISE {
bool FireProductionForceContextResourceQualification(unsigned int mode);
bool FireProductionProjectionContextResourceQualification(unsigned int mode);
}
int main(int argc,char** argv)
{
    using Probe=bool(*)(unsigned int);
    const std::array<Probe,2> probes={{RISE::FireProductionForceContextResourceQualification,
        RISE::FireProductionProjectionContextResourceQualification}};
    unsigned int start=0u,end=2u;
    if(argc==2&&std::strcmp(argv[1],"projection")==0){start=1u;end=2u;}
    else if(argc!=1)return 7;
    for(unsigned int i=start;i<end;++i){const auto probe=probes[i];
        if(!probe(0u))return 2;
        if(probe(1u))return 3;
        if(!probe(2u))return 4;
        if(!probe(3u))return 5;
        if(probe(4u))return 6;
    }
    return 0;
}
