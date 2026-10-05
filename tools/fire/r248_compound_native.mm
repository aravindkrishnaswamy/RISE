#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
struct Record {uint32_t kind,power,positive[33],negative[33],count,positiveCount,negativeCount,zeroCount;};
struct Params {uint32_t count,groups,power,kind;};
static_assert(sizeof(Record)==288,"exact limb wire");
static uint64_t rss(){mach_task_basic_info_data_t i{};mach_msg_type_number_t n=MACH_TASK_BASIC_INFO_COUNT;
 return task_info(mach_task_self(),MACH_TASK_BASIC_INFO,reinterpret_cast<task_info_t>(&i),&n)==KERN_SUCCESS?i.resident_size:0;}
static uint64_t peak(){rusage r{};return getrusage(RUSAGE_SELF,&r)==0?static_cast<uint64_t>(r.ru_maxrss):0;}
static NSString* sha(const void* bytes,size_t n){unsigned char d[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes,static_cast<CC_LONG>(n),d);
 NSMutableString* s=[NSMutableString string];for(unsigned char x:d)[s appendFormat:@"%02x",x];return s;}
static int report(NSMutableDictionary* d,NSString* status,int code){d[@"status"]=status;d[@"exit_code"]=@(code);
 d[@"current_RSS_bytes"]=@(rss());d[@"peak_RSS_bytes"]=@(peak());NSError* e=nil;
 NSData* raw=[NSJSONSerialization dataWithJSONObject:d options:0 error:&e];if(!raw){fprintf(stderr,"report encoding failed\n");return 9;}
 if(fwrite(raw.bytes,1,raw.length,stdout)!=raw.length||fputc('\n',stdout)==EOF||fflush(stdout)!=0){fprintf(stderr,"report publication failed\n");return 9;}return code;}
int main(int argc,char** argv){@autoreleasepool{
 NSMutableDictionary* r=[@{@"schema":@"rise.fire.r248.native-compound-control.v1",@"native_observer_integrated":@NO,@"full_history_authenticated":@NO,@"new_simulation_launched":@NO} mutableCopy];
 if(argc!=7&&argc!=8)return report(r,@"requires shader input output budget_bytes order kind",2);
 bool primitiveControl=argc==8;if(primitiveControl&&strcmp(argv[7],"--synthetic-add")!=0)return report(r,@"invalid synthetic primitive control",2);
 r[@"synthetic_primitive_control"]=@(primitiveControl);
 char* end=nullptr;unsigned long long budget=strtoull(argv[4],&end,10);
 if(!end||*end||argv[4][0]=='-'||budget==0||budget>268435456ull)return report(r,@"invalid bounded fixture budget",2);
 char* powerEnd=nullptr;unsigned long power=strtoul(argv[5],&powerEnd,10);
 if(!powerEnd||powerEnd==argv[5]||*powerEnd||argv[5][0]=='-'||power>2)return report(r,@"invalid coordinate moment order",2);
 char* kindEnd=nullptr;unsigned long kind=strtoul(argv[6],&kindEnd,10);
 if(!kindEnd||kindEnd==argv[6]||*kindEnd||argv[6][0]=='-'||kind>1)return report(r,@"invalid compound kind",2);
 r[@"compound_kind"]=@(kind);
 r[@"coordinate_moment_order"]=@(power);
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return report(r,@"Metal unavailable",3);
 struct stat shaderStat{};
 if(lstat(argv[1],&shaderStat)!=0||!S_ISREG(shaderStat.st_mode)||shaderStat.st_size<=0||shaderStat.st_size>1048576)return report(r,@"bounded shader capture refused",2);
 int shaderFD=open(argv[1],O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat shaderOpened{};
 if(shaderFD<0||fstat(shaderFD,&shaderOpened)!=0||!S_ISREG(shaderOpened.st_mode)||shaderOpened.st_dev!=shaderStat.st_dev||shaderOpened.st_ino!=shaderStat.st_ino||shaderOpened.st_size!=shaderStat.st_size){if(shaderFD>=0)close(shaderFD);return report(r,@"shader capture identity refused",2);}
 std::vector<unsigned char> shaderBytes(static_cast<size_t>(shaderStat.st_size));size_t shaderOffset=0;
 while(shaderOffset<shaderBytes.size()){ssize_t count=read(shaderFD,shaderBytes.data()+shaderOffset,shaderBytes.size()-shaderOffset);if(count<=0){close(shaderFD);return report(r,@"shader capture incomplete",2);}shaderOffset+=static_cast<size_t>(count);}
 if(close(shaderFD)!=0)return report(r,@"shader capture close failed",2);
 NSData* source=[NSData dataWithBytes:shaderBytes.data() length:shaderBytes.size()];
 r[@"captured_shader_sha256"]=sha(source.bytes,source.length);
 NSString* text=[[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding];if(!text)return report(r,@"invalid shader UTF8",2);
 NSError* error=nil;MTLCompileOptions* options=[MTLCompileOptions new];options.fastMathEnabled=NO;
 id<MTLLibrary> library=[device newLibraryWithSource:text options:options error:&error];
 if(!library){r[@"error"]=error.localizedDescription?:@"unknown";return report(r,@"Metal compilation failed",4);}
 id<MTLComputePipelineState> leaves=[device newComputePipelineStateWithFunction:[library newFunctionWithName:primitiveControl?@"control_add":@"reduce_leaves"] error:&error];
 id<MTLComputePipelineState> partials=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"reduce_partials"] error:&error];
 if(!leaves||!partials)return report(r,@"pipeline failed",4);
 struct stat st{};if(lstat(argv[2],&st)!=0||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size%16||static_cast<uint64_t>(st.st_size)>2147483648ull)return report(r,@"regular bounded uint32 input refused",2);
 uint64_t n=static_cast<uint64_t>(st.st_size)/16,a=(n+255)/256,b=(a+255)/256;
 if(primitiveControl&&n!=1)return report(r,@"synthetic primitive requires one selector",2);
 uint64_t inputBytes=n*16,bankA=a*288,bankB=b*288,deviceBytes=inputBytes+bankA+bankB+4;
 uint64_t hostBytes=inputBytes+288+source.length*3+sizeof(Params),baseline=rss();
 r[@"before_field_inventory"]=@{@"input_host_capture_bytes":@(inputBytes),@"shader_capture_byte_and_UTF8_estimate":@(source.length*3),@"output_host_bytes":@288,@"params_bytes":@(sizeof(Params)),@"metal_input_bytes":@(inputBytes),@"metal_bank_a_bytes":@(bankA),@"metal_bank_b_bytes":@(bankB),@"metal_failure_bytes":@4,@"dynamic_threadgroup_bytes":@4608,@"baseline_RSS_bytes":@(baseline),@"device_current_allocated_bytes":@(device.currentAllocatedSize),@"max_buffer_length":@(device.maxBufferLength),@"max_threadgroup_memory":@(device.maxThreadgroupMemoryLength),@"static_threadgroup_bytes":@(MAX(leaves.staticThreadgroupMemoryLength,partials.staticThreadgroupMemoryLength)),@"fixture_budget_bytes":@(budget),@"register_spill_measured":@NO};
 r[@"field_buffers_allocated"]=@NO;
 if(baseline==0||n>UINT32_MAX||inputBytes>device.maxBufferLength||bankA>device.maxBufferLength||bankB>device.maxBufferLength||
   leaves.maxTotalThreadsPerThreadgroup<16||partials.maxTotalThreadsPerThreadgroup<16||
   4608+MAX(leaves.staticThreadgroupMemoryLength,partials.staticThreadgroupMemoryLength)>device.maxThreadgroupMemoryLength||
   hostBytes+deviceBytes>budget||baseline+hostBytes+deviceBytes>budget||device.currentAllocatedSize+deviceBytes>budget)
   return report(r,@"before-field allocation refusal",5);
 int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat opened{};
 if(fd<0||fstat(fd,&opened)!=0||!S_ISREG(opened.st_mode)||opened.st_dev!=st.st_dev||opened.st_ino!=st.st_ino||opened.st_size!=st.st_size){if(fd>=0)close(fd);return report(r,@"input capture identity refused",2);}
 std::vector<uint32_t> bits(n*4);size_t offset=0;while(offset<inputBytes){ssize_t count=read(fd,reinterpret_cast<char*>(bits.data())+offset,inputBytes-offset);if(count<=0){close(fd);return report(r,@"input capture incomplete",2);}offset+=static_cast<size_t>(count);}close(fd);
 r[@"captured_input_sha256"]=sha(bits.data(),inputBytes);
 id<MTLBuffer> input=[device newBufferWithBytes:bits.data() length:inputBytes options:MTLResourceStorageModeShared];
 id<MTLBuffer> first=[device newBufferWithLength:bankA options:MTLResourceStorageModeShared];
 id<MTLBuffer> second=[device newBufferWithLength:bankB options:MTLResourceStorageModeShared];
 id<MTLBuffer> failure=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
 if(!input||!first||!second||!failure)return report(r,@"actual buffer allocation failed",6);
 r[@"field_buffers_allocated"]=@YES;*static_cast<uint32_t*>(failure.contents)=0;
 r[@"device_allocated_after_buffers_bytes"]=@(device.currentAllocatedSize);
 r[@"RSS_after_buffers_bytes"]=@(rss());
 if(rss()==0||rss()>budget||device.currentAllocatedSize>budget)return report(r,@"measured post-field budget refusal",5);
 id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> cmd=[queue commandBuffer];
 if(!queue||!cmd)return report(r,@"command allocation failed",6);
 id<MTLBuffer> in=input,out=first;uint32_t count=static_cast<uint32_t>(n);bool leafPass=true;
 do{uint32_t groups=static_cast<uint32_t>((static_cast<uint64_t>(count)+255)/256);Params p{count,groups,static_cast<uint32_t>(power),static_cast<uint32_t>(kind)};
  id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];if(!enc)return report(r,@"encoder failed",6);
  [enc setComputePipelineState:leafPass?leaves:partials];[enc setBuffer:in offset:0 atIndex:0];[enc setBuffer:out offset:0 atIndex:1];
  [enc setBuffer:failure offset:0 atIndex:2];[enc setBytes:&p length:sizeof(p) atIndex:3];[enc setThreadgroupMemoryLength:4608 atIndex:0];
  [enc dispatchThreadgroups:MTLSizeMake(groups,1,1) threadsPerThreadgroup:MTLSizeMake(16,1,1)];[enc endEncoding];
  count=groups;in=out;out=out==first?second:first;leafPass=false;
 }while(count>1);
 [cmd commit];[cmd waitUntilCompleted];r[@"kernel_failure_bits"]=@(*static_cast<uint32_t*>(failure.contents));
 if(cmd.status!=MTLCommandBufferStatusCompleted)return report(r,@"device command failed",7);
 if(*static_cast<uint32_t*>(failure.contents))return report(r,@"native nonfinite/carry/count refusal",8);
 if(peak()>budget||rss()>budget||device.currentAllocatedSize>budget)return report(r,@"measured completion budget refusal",5);
 Record result{};memcpy(&result,in.contents,sizeof(result));r[@"result_sha256"]=sha(&result,sizeof(result));
 int output=open(argv[3],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(output<0)return report(r,@"exclusive result path refused",2);
 ssize_t written=write(output,&result,sizeof(result));int closed=close(output);if(written!=sizeof(result)||closed!=0)return report(r,@"result write failed",2);
 r[@"accepted_record_bytes"]=@288;return report(r,@"standalone native compound control complete",0);
}}
