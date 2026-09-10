// Offline-assisted discovery. O_RDONLY only; outputs matching aligned addresses.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <string>
int main(int argc,char**argv){
 if(argc!=6)return 2;
 char* end=nullptr;const auto pid=strtoull(argv[1],&end,10);if(!pid||*end)return 2;
 // A bounded comma-separated set reuses one memory pass for related types.
 std::vector<uint64_t> values;
 const char* cursor=argv[2];
 while(*cursor){
  const auto value=strtoull(cursor,&end,16);
  if(end==cursor||!value||(*end&&*end!=',')||values.size()>=64)return 2;
  values.push_back(value);
  if(!*end)break;
  cursor=end+1;if(!*cursor)return 2;
 }
 if(values.empty())return 2;
 std::sort(values.begin(),values.end());
 const auto low=strtoull(argv[3],&end,16);if(*end)return 2;
 const auto high=strtoull(argv[4],&end,16);if(*end||high<=low)return 2;
 const auto mb=strtoull(argv[5],&end,10);if(*end||!mb||mb>1024)return 2;
 char path[80];snprintf(path,sizeof(path),"/proc/%llu/maps",pid);FILE*f=fopen(path,"r");if(!f)return 3;
 snprintf(path,sizeof(path),"/proc/%llu/mem",pid);const int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0){fclose(f);return 3;}
 std::vector<unsigned char> buf(1024*1024);unsigned long long read_bytes=0,requested=0,hits=0,errors=0;
 char line[2048];const auto budget=mb*1024*1024;
 while(fgets(line,sizeof(line),f)&&requested<budget&&hits<128){
  unsigned long long a=0,b=0;char perms[5]{};
  if(sscanf(line,"%llx-%llx %4s",&a,&b,perms)!=3||perms[0]!='r'||perms[1]!='w'||perms[3]!='p')continue;
  a=std::max(a,low);b=std::min(b,high);if(a>=b)continue;
  for(auto p=a;p<b&&requested<budget&&hits<128;){
   const auto n=std::min<unsigned long long>({buf.size(),b-p,budget-requested});requested+=n;
   const auto got=pread(fd,buf.data(),n,p);
   if(got>0){read_bytes+=got;for(size_t i=0;i+8<=static_cast<size_t>(got);i+=8){
    uint64_t v=0;memcpy(&v,buf.data()+i,8);if(std::binary_search(values.begin(),values.end(),v)){
     if(values.size()==1)printf("candidate=0x%llx\n",p+i);
     else printf("candidate=0x%llx value=0x%llx\n",p+i,static_cast<unsigned long long>(v));
     if(++hits>=128)break;
    }
   }}else ++errors;p+=n;
  }
 }
 fclose(f);close(fd);printf("requested=%llu read=%llu hits=%llu errors=%llu truncated=%d game_writes=0\n",requested,read_bytes,hits,errors,requested>=budget||hits>=128);return 0;
}
