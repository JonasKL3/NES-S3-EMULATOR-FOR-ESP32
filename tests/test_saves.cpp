#include "halo_savestates.h"
#include "halo_library.h"
#include "nes/nes.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>
#include <iostream>
namespace fs=std::filesystem;
static rominfo_t rominfo{};
static nes_t nes{&rominfo};
extern "C" nes_t *nes_getcontextptr(){return &nes;}
static int selected=0;
extern "C" void state_setslot(int s){selected=s;}
extern "C" int state_save(){
 std::string p=nes.rominfo->filename; auto d=p.rfind(".nes");
 if(d==std::string::npos)return -1;
 p.replace(d,4,".ss"+std::to_string(selected));
 std::ofstream f(p,std::ios::binary|std::ios::trunc);
 unsigned char head[]={ 'S','N','S','S',0,0,0,1,'B','A','S','R',0,0,0,1,0,0,0,100};
 f.write((char*)head,sizeof(head));char data[100]={};f.write(data,sizeof(data));
 return f.good()?0:-1;
}
extern "C" int state_load(){
 std::string p=nes.rominfo->filename;auto d=p.rfind(".nes");
 if(d==std::string::npos)return -1;
 p.replace(d,4,".ss"+std::to_string(selected));
 return std::filesystem::exists(p)?0:-1;
}
std::string dir=TEST_SAVE_DIR;
std::string path(int slot=0,std::string variant="") {return dir+"/"+HaloSave::gameKey()+variant+".ss"+std::to_string(slot);}
void createValid(const std::string&p){
 std::ofstream f(p,std::ios::binary|std::ios::trunc);
 unsigned char head[]={ 'S','N','S','S',0,0,0,1,'B','A','S','R',0,0,0,1,0,0,0,100};
 f.write((char*)head,sizeof(head));char data[100]={};f.write(data,sizeof(data));
}
void createBad(const std::string&p){std::ofstream f(p,std::ios::binary|std::ios::trunc); f<<"CORRUPT";}
void clean(){for(auto &e:fs::directory_iterator(dir))fs::remove(e.path());}
int main(){
 fs::create_directories(dir);HaloSave::selectGame("/NES/Demo.nes",0x12345678);
 const std::string a=path(),tmp=path(0,"_TMP"),bak=a+".bak";
 clean();createValid(a);assert(HaloSave::present(0));assert(fs::exists(a));std::cout<<"[PASS] primary valid\n";
 clean();createValid(bak);assert(HaloSave::present(0));assert(fs::exists(a));assert(!fs::exists(bak));std::cout<<"[PASS] backup recovered\n";
 clean();createValid(tmp);assert(HaloSave::present(0));assert(fs::exists(a));assert(!fs::exists(tmp));std::cout<<"[PASS] temp complete recovered\n";
 clean();createBad(a);createValid(bak);createBad(a+".bad0");assert(HaloSave::present(0));assert(fs::exists(a+".bad0")&&fs::exists(a+".bad1"));std::cout<<"[PASS] corrupt protected, no overwrite .bad0\n";
 clean();createBad(a);createValid(tmp);assert(HaloSave::present(0));assert(fs::exists(a+".bad0"));std::cout<<"[PASS] temp fallback with corrupt main\n";
 clean();createBad(tmp);assert(!HaloSave::present(0));assert(fs::exists(tmp));std::cout<<"[PASS] corrupt temp not promoted\n";
 clean();assert(!HaloSave::present(0));std::cout<<"[PASS] absent not recovered\n";
 clean();assert(HaloSave::save(0));assert(HaloSave::present(0));assert(HaloSave::load(0));
 assert(HaloSave::save(0));assert(fs::exists(bak));assert(HaloSave::present(0));
 std::cout<<"[PASS] native save and load via patched rom filename; backup preserved\n";
 clean();assert(HaloLibrary::load()==false);
 HaloLibrary::toggleFavorite(0x00FACADE);HaloLibrary::opened(0xABCDEF01);HaloLibrary::setVolume(45);
 assert(HaloLibrary::isFavorite(0x00FACADE));assert(HaloLibrary::lastGame()==0xABCDEF01);assert(HaloLibrary::volume()==45);
 assert(HaloLibrary::load());assert(HaloLibrary::isFavorite(0x00FACADE));assert(HaloLibrary::recentCount()==1);assert(HaloLibrary::volume()==45);
 std::cout<<"[PASS] HALO_LIB roundtrip and checksum\n";
 // Regressao RC2: nunca substituir estado principal corrompido sem fallback.
 clean();createBad(a);assert(!HaloSave::save(0));assert(fs::exists(a));
 {std::ifstream f(a);std::string v;f>>v;assert(v=="CORRUPT");}
 std::cout<<"[PASS] saves existentes invalidos permanecem intactos\n";
 // Backup antigo e estado valido: nova gravação conserva a transacao por rename.
 clean();createValid(a);createValid(bak);assert(HaloSave::save(0));
 assert(fs::exists(a)&&fs::exists(bak));assert(HaloSave::present(0));
 std::cout<<"[PASS] gravação valida com backup anterior existente\n";
 // HALO_LIB corrompido deve cair para backup conhecido bom.
 auto library=dir+"/HALO_LIB.DAT",backup=dir+"/HALO_LIB.BAK";
 clean();assert(HaloLibrary::save());fs::copy_file(library,backup);
 createBad(library);assert(HaloLibrary::load());
 assert(HaloLibrary::lastGame()==0xABCDEF01 && HaloLibrary::volume()==45);
 std::cout<<"[PASS] HALO_LIB corrompido volta ao backup verificado\n";
 // Nao descartar o backup valido ao gravar preferencias novas diante de PATH danificado.
 HaloLibrary::setVolume(46);
 assert(fs::exists(backup));
 {std::ifstream f(library);std::string v;f>>v;assert(v=="CORRUPT");}
 assert(HaloLibrary::volume()==46); // atualizado em RAM, persistencia bloqueada por seguranca
 std::cout<<"[PASS] backup de favoritos protegido contra principal invalido\n";
 return 0;
}
