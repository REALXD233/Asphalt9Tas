// Explicit-address discovery only. Array candidates are NOT validated Bullet ABI.
// No hooks, target writes, native method calls or process suspension.
#include "remote_data_address_v1.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <climits>

static bool Read(int fd, std::uint64_t address, void* out, size_t bytes) {
    address=a9tas::remote_data_address_v1::Untag(address);
    return address<=INT64_MAX-bytes && pread(fd,out,bytes,address)==static_cast<ssize_t>(bytes);
}
static bool Number(const char* s, int base, unsigned long long* out) {
    if (!s || !*s || *s=='-') return false;
    char* end=nullptr; errno=0; *out=strtoull(s,&end,base);
    return !errno && end!=s && !*end;
}
static unsigned long long StartTicks(unsigned long long pid) {
    char path[64],buf[4096]{};
    std::snprintf(path,sizeof(path),"/proc/%llu/stat",pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    const auto n=read(fd,buf,sizeof(buf)-1);close(fd);if(n<=0)return 0;
    char* tail=std::strrchr(buf,')');if(!tail)return 0;
    char* save=nullptr;char* token=strtok_r(tail+1," \n",&save);
    for(int field=3;token && field<22;field++)token=strtok_r(nullptr," \n",&save);
    unsigned long long value=0;return token&&Number(token,10,&value)?value:0;
}
int main(int argc,char** argv) {
    unsigned long long pid=0,world=0,head_limit=8;
    if((argc!=3 && argc!=4) || !Number(argv[1],10,&pid) || pid==0 || pid>INT_MAX ||
       (argc==4 && (!Number(argv[3],10,&head_limit)||head_limit<1||head_limit>256)) ||
       !Number(argv[2],16,&world) || !world) {
        std::fprintf(stderr,"usage: probe PID VERIFIED_WORLD_HEX [HEAD_LIMIT_1_TO_256]\n");return 2;
    }
    const auto start_ticks=StartTicks(pid);
    if(!start_ticks){std::fprintf(stderr,"process identity unavailable\n");return 3;}
    char path[64];std::snprintf(path,sizeof(path),"/proc/%llu/mem",pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0){std::perror("open mem");return 3;}
    std::uint8_t head[1024]{};
    if(!Read(fd,world,head,sizeof(head))){std::perror("read world");close(fd);return 4;}
    // Dump raw bounded header: offline analysis can verify alternative layouts.
    std::printf("{\"schema\":\"A9WORLD1\",\"pid\":%llu,\"start_ticks\":%llu,\"world\":\"%llx\",\"header_hex\":\"",pid,start_ticks,world);
    for(auto x:head)std::printf("%02x",x);
    std::printf("\",\"array_candidates\":[");
    bool first=true;
    // Candidate layout: int size, int capacity, pointer data. Alignment and
    // bounds are discovery filters, not proof of membership or semantic type.
    for(size_t offset=0;offset+24<=sizeof(head);offset+=8){
        std::int32_t size=0,capacity=0;std::uint64_t data=0;
        std::memcpy(&size,head+offset,4);std::memcpy(&capacity,head+offset+4,4);
        std::memcpy(&data,head+offset+8,8);
        const char* layout="size_capacity_data";
        if(size<=0||capacity<size||capacity>1000000||!data||(data&7)){
            std::uint64_t begin=0,end=0,limit=0;
            std::memcpy(&begin,head+offset,8);std::memcpy(&end,head+offset+8,8);
            std::memcpy(&limit,head+offset+16,8);
            if(!begin||(begin&7)||end<=begin||limit<end||limit-begin>8000000||
               (end-begin)%8||(limit-begin)%8)continue;
            data=begin;size=(end-begin)/8;capacity=(limit-begin)/8;
            layout="begin_end_capacity";
        }
        std::uint64_t pointers[256]{};const int count=size<static_cast<int>(head_limit)?size:static_cast<int>(head_limit);
        if(!Read(fd,data,pointers,count*8))continue;
        if(!first)std::printf(",");first=false;
        std::printf("{\"offset\":%zu,\"layout\":\"%s\",\"size\":%d,\"capacity\":%d,\"data\":\"%llx\",\"heads\":[",offset,layout,size,capacity,(unsigned long long)data);
        for(int i=0;i<count;i++){
            std::uint64_t vptr=0;bool ok=pointers[i]&&Read(fd,pointers[i],&vptr,8);
            std::uint8_t object_head[384]{};
            const bool head_ok=ok&&Read(fd,pointers[i],object_head,sizeof(object_head));
            std::printf("%s{\"pointer\":\"%llx\",\"readable\":%s,\"vptr\":\"%llx\",\"head_hex\":\"",i?",":"",(unsigned long long)pointers[i],ok?"true":"false",(unsigned long long)vptr);
            if(head_ok)for(auto x:object_head)std::printf("%02x",x);
            std::printf("\",\"linked_heads\":[");
            // Discovery fields supported by current ARM64 wrapper getter bodies.
            // Still report raw data only; do not label shape/rigidbody semantics.
            for(int k=0;k<2;k++){
                const size_t field=k==0?0x50:0x90;
                std::uint64_t linked=0;std::uint8_t linked_head[384]{};
                if(head_ok)std::memcpy(&linked,object_head+field,8);
                const bool linked_ok=linked&&Read(fd,linked,linked_head,sizeof(linked_head));
                std::printf("%s{\"field\":%zu,\"pointer\":\"%llx\",\"head_hex\":\"",k?",":"",field,(unsigned long long)linked);
                if(linked_ok)for(auto x:linked_head)std::printf("%02x",x);
                std::uint64_t shape=0;std::uint8_t shape_head[128]{};
                if(linked_ok)std::memcpy(&shape,linked_head+0xd0,8);
                bool shape_ok=shape&&Read(fd,shape,shape_head,sizeof(shape_head));
                std::printf("\",\"shape_candidate\":\"%llx\",\"shape_head_hex\":\"",(unsigned long long)shape);
                if(shape_ok)for(auto x:shape_head)std::printf("%02x",x);
                std::printf("\"}");
            }
            std::printf("]}");
        }
        std::printf("]}");
    }
    std::uint8_t after[1024]{};bool stable=Read(fd,world,after,sizeof(after))&&std::memcmp(head,after,sizeof(head))==0;
    const bool same_process=StartTicks(pid)==start_ticks;
    close(fd);std::printf("],\"header_stable\":%s,\"same_process\":%s,\"abi_verified\":false}\n",stable?"true":"false",same_process?"true":"false");
    return 0;
}
