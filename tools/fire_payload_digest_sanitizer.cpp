// Standalone CPU digest lifetime/parallelism gate; use ASan + UBSan.
#include "../src/Library/Utilities/FireProductionAdvection.h"
#include <cstdio>

int main()
{
	std::string error;
	for(std::size_t bytes:{0u,1u,4095u,4096u,4097u,65537u,1048577u}){
		std::vector<unsigned char> payload(bytes);
		for(std::size_t i=0u;i<bytes;++i)payload[i]=static_cast<unsigned char>((i*37u+(i>>8u)*19u+11u)&255u);
		RISE::FireProductionPayloadDigestV2 serial,parallel;
		if(!RISE::FireProductionPayloadDigestCPU(payload.data(),bytes,1u,serial,&error))return 1;
		for(unsigned int workers:{3u,8u,256u}){
			if(!RISE::FireProductionPayloadDigestCPU(payload.data(),bytes,workers,parallel,&error)||
				serial.rootSHA256!=parallel.rootSHA256)return 2;
		}
		if(bytes){payload.back()^=128u;
			if(!RISE::FireProductionPayloadDigestCPU(payload.data(),bytes,8u,parallel,&error)||
				serial.rootSHA256==parallel.rootSHA256)return 3;}
	}
	std::puts("MERKLE_V2_SANITIZER_PASS");return 0;
}
