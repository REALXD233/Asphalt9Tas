// Bounded external memory reader. No target writes or method calls.
#include "remote_data_address_v1.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char** argv){
 if(argc!=4)return 2;
 char* end=nullptr;errno=0;long pid=strtol(argv[1],&end,10);
 if(errno||!end||*end||pid<=0||pid>2147483647)return 2;
 errno=0;auto address=strtoull(argv[2],&end,16);if(errno||!end||*end||!address)return 2;
 errno=0;auto size=strtoul(argv[3],&end,10);if(errno||!end||*end||!size||size>1048576)return 2;
 address=a9tas::remote_data_address_v1::Untag(address);
 if(address>INT64_MAX-size)return 2;
 char path[64];snprintf(path,sizeof(path),"/proc/%ld/mem",pid);int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return 3;
 unsigned char buf[4096];unsigned long done=0;
 while(done<size){auto want=size-done;if(want>sizeof(buf))want=sizeof(buf);auto got=pread(fd,buf,want,address+done);if(got<=0){close(fd);return 4;}for(int i=0;i<got;i++)printf("%02x",buf[i]);done+=got;}
 close(fd);puts("");return 0;
}
