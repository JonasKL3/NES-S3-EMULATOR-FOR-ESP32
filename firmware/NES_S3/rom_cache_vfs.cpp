#include "rom_cache_vfs.h"
#include <Arduino.h>
#include <esp_vfs.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
namespace HaloRomCache {
static uint8_t *data=nullptr;
static size_t dataSize=0;
static bool mounted=false;
static bool openSlots[4]={};
static size_t positions[4]={};
static const char* FILE_PATH="/romcache/GAME.NES";
static constexpr uint32_t FNV0=2166136261u, FNVP=16777619u;
static int openFn(void*,const char* p,int flags,int){
 if(!data || !p || strcmp(p,"/GAME.NES") || (flags&O_ACCMODE)!=O_RDONLY){errno=ENOENT;return -1;}
 for(int i=0;i<4;i++)if(!openSlots[i]){openSlots[i]=true;positions[i]=0;return i;}
 errno=EMFILE;return -1;
}
static ssize_t readFn(void*,int fd,void* out,size_t n){
 if(fd<0||fd>=4||!openSlots[fd]||!data){errno=EBADF;return -1;}
 if(!out && n){errno=EINVAL;return -1;}
 size_t off=positions[fd],rem=off<dataSize?dataSize-off:0;
 if(n>rem)n=rem;
 if(n)memcpy(out,data+off,n);
 positions[fd]=off+n;
 return (ssize_t)n;
}
static off_t seekFn(void*,int fd,off_t n,int whence){
 if(fd<0||fd>=4||!openSlots[fd]||!data){errno=EBADF;return -1;}
 int64_t base=(whence==SEEK_SET)?0:(whence==SEEK_CUR)?(int64_t)positions[fd]:(whence==SEEK_END)?(int64_t)dataSize:-1;
 int64_t next=base+(int64_t)n;
 if(base<0||next<0||next>LONG_MAX){errno=EINVAL;return -1;}
 positions[fd]=(size_t)next;
 return (off_t)next;
}
static int closeFn(void*,int fd){
 if(fd<0||fd>=4||!openSlots[fd]){errno=EBADF;return -1;}
 openSlots[fd]=false;positions[fd]=0;return 0;
}
static int statFill(struct stat* s){
 if(!data){errno=ENOENT;return -1;}
 memset(s,0,sizeof(*s));s->st_mode=S_IFREG|0444;s->st_size=(off_t)dataSize;s->st_nlink=1;return 0;
}
static int fstatFn(void*,int fd,struct stat* s){
 if(fd<0||fd>=4||!openSlots[fd]||!s){errno=EBADF;return -1;}
 return statFill(s);
}
static int statFn(void*,const char* p,struct stat* s){
 if(!p||strcmp(p,"/GAME.NES")||!s){errno=ENOENT;return -1;}
 return statFill(s);
}
bool install(){
 if(mounted)return true;
 esp_vfs_t fs={};fs.flags=ESP_VFS_FLAG_CONTEXT_PTR;
 fs.open_p=openFn;fs.read_p=readFn;fs.lseek_p=seekFn;fs.close_p=closeFn;
 fs.fstat_p=fstatFn;
#ifdef CONFIG_VFS_SUPPORT_DIR
 fs.stat_p=statFn;
#endif
 esp_err_t r=esp_vfs_register("/romcache",&fs,nullptr);
 mounted=(r==ESP_OK);
 Serial.printf("[R9][ROMVFS] register=%d mounted=%u\n",(int)r,(unsigned)mounted);
 return mounted;
}
void release(){
 for(int i=0;i<4;i++){openSlots[i]=false;positions[i]=0;}
 if(data){free(data);data=nullptr;}dataSize=0;
}
size_t size(){return dataSize;}
const char* path(){return FILE_PATH;}
bool load(const char* path,uint32_t* payloadHash,uint32_t* readMs,int* failureErr,uint32_t* readBytes){
 if(!mounted||!path)return false;
 release();
 static uint8_t bounce[2048];
 uint32_t t0=millis();
 errno=0;
 FILE* f=fopen(path,"rb");
 if(!f){if(failureErr)*failureErr=errno?errno:EIO;return false;}
 if(fseek(f,0,SEEK_END)!=0){if(failureErr)*failureErr=errno?errno:EIO;fclose(f);return false;}
 long total=ftell(f);
 if(total<16||total>4*1024*1024L||fseek(f,0,SEEK_SET)!=0){
   if(failureErr)*failureErr=EINVAL;
   fclose(f);
   return false;
 }
 data=(uint8_t*)heap_caps_malloc((size_t)total,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!data){if(failureErr)*failureErr=ENOMEM;fclose(f);return false;}
 dataSize=(size_t)total;
 size_t done=0;
 while(done<dataSize){
   size_t req=dataSize-done;if(req>sizeof(bounce))req=sizeof(bounce);
   errno=0;size_t got=fread(bounce,1,req,f);
   if(got!=req||ferror(f)){
     int e=errno?errno:EIO;
     Serial.printf("[R9][SD][SHORT] offset=%u got=%u req=%u errno=%d\n",(unsigned)done,(unsigned)got,(unsigned)req,e);
     if(failureErr)*failureErr=e;
     if(readBytes)*readBytes=(uint32_t)done;
     fclose(f);release();return false;
   }
   memcpy(data+done,bounce,got);done+=got;
   if((done&0x1FFF)==0)delay(1);
 }
 errno=0;
 if(fclose(f)!=0){
   if(failureErr)*failureErr=errno?errno:EIO;
   if(readBytes)*readBytes=(uint32_t)done;
   release();return false;
 }
 if(readBytes)*readBytes=(uint32_t)done;
 if(readMs)*readMs=millis()-t0;
 if(memcmp(data,"NES\x1A",4)!=0||data[4]==0){if(failureErr)*failureErr=EINVAL;release();return false;}
 uint32_t trainer=(data[6]&4)?512:0;
 uint32_t payload=(uint32_t)data[4]*16384u+(uint32_t)data[5]*8192u;
 if((uint64_t)16+trainer+payload>dataSize){if(failureErr)*failureErr=EINVAL;release();return false;}
 uint32_t h=FNV0;
 for(uint32_t i=16+trainer;i<16+trainer+payload;i++)h=(h^data[i])*FNVP;
 if(payloadHash)*payloadHash=h;
 if(failureErr)*failureErr=0;
 // This is the ONLY data read from SD. Nofrendo sees RAM-only VFS.
 FILE* check=fopen(FILE_PATH,"rb");
 if(!check){if(failureErr)*failureErr=EIO;release();return false;}
 uint8_t m[4];bool ok=fread(m,1,4,check)==4&&memcmp(m,"NES\x1A",4)==0;
 fclose(check);
 if(!ok){if(failureErr)*failureErr=EIO;release();return false;}
 Serial.printf("[R9][SD][CACHE] PSRAM bytes=%u hash=%08lX elapsed=%lu ms\n",
  (unsigned)dataSize,(unsigned long)h,(unsigned long)(millis()-t0));
 return true;
}
}
