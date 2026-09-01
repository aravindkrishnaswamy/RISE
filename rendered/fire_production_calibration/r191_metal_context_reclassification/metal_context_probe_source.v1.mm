#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>

int main()
{
	@autoreleasepool {
		id<MTLDevice> defaultDevice=MTLCreateSystemDefaultDevice();
		NSArray<id<MTLDevice> >* devices=MTLCopyAllDevices();
		std::printf("default_present %d\n",defaultDevice ? 1 : 0);
		if( defaultDevice ) std::printf("default_name %s\ndefault_registry_id %llu\n",
			[[defaultDevice name] UTF8String],
			static_cast<unsigned long long>([defaultDevice registryID]));
		std::printf("all_device_count %llu\n",
			static_cast<unsigned long long>([devices count]));
		for( NSUInteger index=0;index<[devices count];++index ) {
			id<MTLDevice> device=[devices objectAtIndex:index];
			std::printf("device %llu %s %llu\n",
				static_cast<unsigned long long>(index),[[device name] UTF8String],
				static_cast<unsigned long long>([device registryID]));
		}
	}
	return 0;
}
