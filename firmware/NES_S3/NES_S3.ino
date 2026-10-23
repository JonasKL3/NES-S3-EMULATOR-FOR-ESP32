/*
 * NES-S3 - v0.2.9.11-r13.1-demo-rc2 (SEM WIFI)
 * Primeiro firmware com nucleo de emulacao Nofrendo real.
 * Core externo: moononournation/arduino-nofrendo, versao 1.2
 * O proprio projeto NAO inclui ROMs nem distribui a biblioteca de terceiros.
 * R13 CORE LOCKED: revisao conservadora de robustez sem alterar paleta, TFT, audio ou controles.
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>
#include "nes_color.h"
#include "rom_cache_vfs.h"
#include "halo_savestates.h"
#include "halo_library.h"
#include "halo_audio.h"
#include "nes_s3_covers.h"

#define CUSTOM_SETTINGS
#define INCLUDE_GAMEPAD_MODULE
#include <DabbleESP32.h>

extern "C" {
#include <nofrendo.h>
}

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
  #error "Selecione ESP32S3 Dev Module no Arduino IDE"
#endif
#if !defined(USE_HSPI_PORT)
  #error "No TFT_eSPI: habilite #define USE_HSPI_PORT no User_Setup ativo"
#endif
#if defined(TOUCH_CS) && (TOUCH_CS == 14)
  #error "No TFT_eSPI: desative TOUCH_CS 14. GPIO14 pertence ao SD."
#endif
#if !defined(TFT_CS) || (TFT_CS != 10) || !defined(TFT_DC) || (TFT_DC != 9) || !defined(TFT_RST) || (TFT_RST != 4)
  #error "TFT CS/DC/RST devem ser 10/9/4. Confira User_Setup ativo."
#endif
#if !defined(TFT_MOSI) || (TFT_MOSI != 11) || !defined(TFT_MISO) || (TFT_MISO != 13) || !defined(TFT_SCLK) || (TFT_SCLK != 12)
  #error "TFT MOSI/MISO/SCK devem ser 11/13/12. Confira User_Setup ativo."
#endif

static constexpr uint8_t SD_CS_PIN = 14;
static constexpr uint8_t TFT_ROTATION = 2; // Restaurado da v0.1.1: orientacao de referencia da placa
static constexpr uint16_t MAX_GAMES = 500;
static constexpr uint8_t MENU_ROWS = 7;
static constexpr size_t PATH_CAP = 240;
static constexpr size_t NAME_CAP = 65;

struct NesGame {
  char path[PATH_CAP];
  char title[NAME_CAP];
};

TFT_eSPI tft = TFT_eSPI(320, 240); // Mesmo construtor da v0.1.1
NesGame *gameList = nullptr;
uint16_t gameCount = 0;
uint16_t menuSelection = 0;
uint16_t menuTop = 0;
static uint16_t visibleGames[MAX_GAMES]={};
static uint16_t visibleCount=0,visibleSelection=0;
static uint8_t libraryView=0; // 0=TODOS, 1=FAVORITOS, 2=RECENTES
static char librarySearch[13]={};
static bool keyboardOpen=false,resumePrompt=false;
static uint8_t keyboardCursor=0;
bool launchResumeAuto=false;
volatile uint16_t gamepadMask = 0;
volatile bool isEmulating = false;
volatile bool exitRequested = false;
// A tarefa NES consome o pedido entre quadros; so ela acessa o TFT durante pausa.
volatile bool pauseMenuRequested = false;
volatile bool pauseMenuActive = false;
volatile uint16_t pauseMenuButtons = 0;
volatile bool pauseWaitForRelease = false;
// Incrementado no retorno do menu de pausa; sincroniza janela de FPS.
volatile uint32_t pauseResumeEpoch = 0;
volatile bool emulationCompleted = false;
volatile uint32_t producedFrames = 0;
volatile uint32_t emulatorYields = 0;
volatile uint32_t emulatorStackMinFree = UINT32_MAX;
volatile uint32_t videoTransferLastUs = 0;  // somente duracao pushImage() de 1 quadro
volatile uint32_t videoTransferMaxUs = 0;   // pior transferencia da ROM corrente
// PERF por sessao: nao carregar contadores da ROM anterior ao mudar de jogo.
static uint32_t perfPreviousAt=0;
static uint32_t perfPreviousFrames=0;
static uint32_t lastPauseResumeEpoch=0;
// Diagnostico independente: ordem fisica da matriz, bytes de pushImage e paleta NES.
// Perfil da v0.2.7 validado pelo usuario: ALT / SWAP / FIXA.
// Nao alterar isoladamente: cores do ILI9341 dependem das 3 camadas.
volatile bool nesSwapRedBlue = false; // Fixo: nao inverter os canais da paleta
volatile bool nesUseFixedPalette = true; // FIXA: shady_palette, escolha do usuario no painel real
// Laboratorio LCD: so a tarefa NES altera registradores TFT. Padrao = v0.2.9.4.
volatile uint8_t nesLcdAction=0;    // 1 proximo perfil; 2 invert; 3 aspecto; 4 restaurar
volatile uint8_t nesLcdPreset=0;    // 0 padrao, 1 gamma alternativo, 2 gamma+VCOM, 3 IPS gamma
volatile uint8_t nesLcdInversion=0;
volatile uint8_t nesAspect43=0;      // 0 nativo 256x240; 1 expandido 320x240

static constexpr bool panelBgrAlternate = true;  // ALT: inverte bit BGR do MADCTL
static constexpr bool imageSwapBytes = true;    // SWAP: caminho pushImage no TFT_eSPI
// Configuracoes COLOR LOCK: painel ALT / bytes SWAP / pal FIXA. Nao alteraveis por Dabble.
#if defined(M5STACK)
  static constexpr uint8_t ILI9341_ROT2_MADCTL = 0x20 | 0x40;
#else
  static constexpr uint8_t ILI9341_ROT2_MADCTL = 0x80;
#endif
#if defined(TFT_RGB_ORDER) && (TFT_RGB_ORDER == 1)
  static constexpr uint8_t ILI9341_DEFAULT_COLOR_BIT = 0x00;
#else
  static constexpr uint8_t ILI9341_DEFAULT_COLOR_BIT = 0x08;
#endif

static char selectedStdIoPath[PATH_CAP + 8];
// Hash FNV-1a do payload PRG+CHR obtido em uma primeira leitura completa.
// O patch NES_ROM_INTEGRITY_V0293 compara este valor aos bancos realmente
// carregados na PSRAM pelo Nofrendo. NUNCA permite jogar com bancos distintos.
volatile uint32_t nesExpectedPayloadHash=0;
volatile bool nesExpectedPayloadReady=false;
volatile uint32_t nesPayloadValidationCalls=0;
static uint32_t lastRomHash=0;
// Verificacao diagnostica do buffer real do NES (nao altera imagem).
extern volatile uint32_t videoSampleNonzeroPercent;
extern volatile uint32_t videoSampleUniqueColors;
extern volatile uint32_t videoSampleHash;
extern volatile uint32_t videoSamplesMade;

// Erros do SD confirmados no hardware: EIO (5), ENODEV (19).
// A recuperacao so e acionada no menu; nunca reinicializar SD enquanto NES
// usa o FatFS para ROM ou save SRAM. Nao alterar frequencia sem validar placa.
static constexpr uint32_t SD_SAFE_FREQ_HZ=1000000;
static constexpr uint8_t SD_BOOT_RETRIES=3;
static constexpr uint8_t SD_RECOVERY_RETRIES=2;
static uint32_t sdRecoveryCount=0;
static uint32_t sdReadFailures=0;
static uint32_t sdReadMsLast=0;
static bool sdCatalogVerified=false;
static bool sdOnline=false;

static bool sdErrorIsDeviceFault(int err) {
  // EBADF sinaliza descritor invalido; sozinho nao comprova falha do cartao.
  return err==EIO || err==ENODEV || err==ENXIO;
}
// CS do TFT em HIGH antes de conversar com o SD. Sem SPIClass novo.
static bool mountSD(uint8_t maxAttempts,const char*reason) {
  for(uint8_t a=1;a<=maxAttempts;a++) {
    digitalWrite(TFT_CS,HIGH);
    digitalWrite(SD_CS_PIN,HIGH);
    bool ok=SD.begin(SD_CS_PIN,tft.getSPIinstance(),SD_SAFE_FREQ_HZ,"/sd",5,false);
    Serial.printf("[SD][%s] tentativa=%u/%u resultado=%s\n",reason,(unsigned)a,(unsigned)maxAttempts,ok?"OK":"FAIL");
    if(ok){sdOnline=true;return true;}
    delay(200);
  }
  sdOnline=false;
  return false;
}
// Uma montagem OK nao prova acesso aos arquivos. Verificar a pasta REAL.
// Medimos isso antes e depois de inicializar o TFT, para identificar se o
// compartilhamento do SPI e responsavel pelo erro intermitente.
static bool probeNESDirectory(const char*stage) {
  errno=0;
  File dir=SD.open("/NES",FILE_READ);
  const bool ok=(dir && dir.isDirectory());
  const int openErr=errno; // diagnostico; a API FS nem sempre popula errno.
  if(dir)dir.close();
  Serial.printf("[SD][PROBE][%s] /NES=%s errno=%d\n",stage,ok?"OK":"FAIL",openErr);
  return ok;
}
static bool recoverSDOnMenu(int triggeringErr) {
  // Nenhuma ROM pode estar usando FatFS durante desmontagem. Nao usamos
  // reboot, criacao de SPIClass secundario nem reconfiguracao dos pinos.
  if(isEmulating){Serial.println("[SD][RECOVER][BLOCK] emulacao ativa");return false;}
  ++sdReadFailures;
  Serial.printf("[SD][RECOVER] errno=%d inicio - verificando SD (contador=%lu)\n",
                triggeringErr,(unsigned long)(sdRecoveryCount+1));
  for(uint8_t cycle=1;cycle<=SD_RECOVERY_RETRIES;cycle++) {
    SD.end();
    sdOnline=false;
    digitalWrite(TFT_CS,HIGH);
    digitalWrite(SD_CS_PIN,HIGH);
    delay(150*cycle);
    if(!mountSD(1,"RECOVER"))continue;
    if(probeNESDirectory("REMOUNT")){
      ++sdRecoveryCount;
      return true;
    }
    Serial.printf("[SD][RECOVER] tentativa %u/%u montou mas /NES permanece inacessivel\n",
                  (unsigned)cycle,(unsigned)SD_RECOVERY_RETRIES);
  }
  sdOnline=false; // estado logico: SD/FAT nao pode ser considerado pronto.
  Serial.println("[SD][RECOVER][FAIL] acesso a /NES nao restabelecido. Verificar CS/MISO/alimentacao/cartao");
  return false;
}


// Input bitmask usado pelo osd_port.cpp (A,B,Select,Start,Up,Down,Left,Right).
static uint16_t readDabbleButtons() {
  uint16_t m = 0;
  const bool left = GamePad.isLeftPressed();
  const bool right = GamePad.isRightPressed();
  const bool up = GamePad.isUpPressed();
  const bool down = GamePad.isDownPressed();
  const int x = GamePad.getXaxisData();
  const int y = GamePad.getYaxisData();
  if (left != right) { if (left) m |= 0x40; else m |= 0x80; }
  else if (!left && !right) { if (x < -2) m |= 0x40; else if (x > 2) m |= 0x80; }
  if (up != down) { if (up) m |= 0x10; else m |= 0x20; }
  else if (!up && !down) { if (y > 2) m |= 0x10; else if (y < -2) m |= 0x20; }
  if (GamePad.isCrossPressed() || GamePad.isCirclePressed()) m |= 0x01; // NES A
  if (GamePad.isSquarePressed() || GamePad.isTrianglePressed()) m |= 0x02; // NES B
  if (GamePad.isSelectPressed()) m |= 0x04;
  if (GamePad.isStartPressed()) m |= 0x08;
  return m;
}

static void applyColorPipeline(const char *reason) {
  // MADCTL bit 3 controla RGB/BGR no controlador fisico: nao e o mesmo
  // que swap de bytes nem a troca R/B da paleta do Nofrendo.
  const uint8_t colorBit=(uint8_t)(ILI9341_DEFAULT_COLOR_BIT ^ (panelBgrAlternate ? 0x08 : 0));
  const uint8_t madctl=(uint8_t)(ILI9341_ROT2_MADCTL | colorBit);
  tft.writecommand(0x36);  // ILI9341 MADCTL
  tft.writedata(madctl);
  tft.setSwapBytes(imageSwapBytes);
  Serial.printf("[COLOR][PIPE] %s MADCTL=0x%02X %s pushImageSwap=%u NESfixed=%u | rotation=%u\n",
    reason,madctl,panelBgrAlternate?"ALTERADO":"PADRAO",(unsigned)imageSwapBytes,
    (unsigned)__atomic_load_n(&nesUseFixedPalette,__ATOMIC_ACQUIRE),TFT_ROTATION);
}

static void reportMemory(const char *step) {
  Serial.printf("[MEM][%s] heap=%u psram=%u max_internal=%u\n", step,
    (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram(),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
}

// Splash vetorial na propria flash: evita abertura/leituras extras no SD.
// A tela so pode ser inicializada DEPOIS do SD, por causa do modo SPI compartilhado.
// A marca NES-S3 e desenhada em flash, sem acessar o microSD.
// Nenhum carregamento de capa ocorre durante a emulacao.
static void drawHaloBoot(){
  tft.fillScreen(0x000B);
  tft.drawRoundRect(38,53,244,132,15,0x03FF);
  tft.drawRoundRect(41,56,238,126,12,0x023F);
  tft.fillRoundRect(59,83,202,60,10,0x000B);
  tft.setTextColor(TFT_CYAN,0x000B);
  tft.setTextSize(4);tft.setCursor(74,95);tft.print("NES-S3");
  tft.drawFastHLine(80,157,160,0x03FF);
  tft.setTextSize(1);tft.setTextColor(TFT_WHITE,0x000B);
  tft.setCursor(80,166);tft.print("RETRO SYSTEM  ESP32-S3");
}
static void screenMessage(const char *line1,const char *line2="") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2); tft.setTextColor(TFT_CYAN,TFT_BLACK); tft.setCursor(10,16); tft.print("NES-S3");
  tft.setTextSize(1); tft.setTextColor(TFT_WHITE,TFT_BLACK); tft.setCursor(12,78); tft.print(line1);
  tft.setCursor(12,100); tft.setTextColor(TFT_YELLOW,TFT_BLACK);tft.print(line2);
}

static bool isNesName(const char *name) {
  if (!name) return false;
  const size_t n = strlen(name);
  return n >= 4 && strcasecmp(name+n-4,".nes") == 0;
}

// So enumera nomes. Nao abre ROM por ROM: resolve boot de 134 segundos da versao anterior.
static bool scanGameNames() {
  gameCount = 0; menuSelection = menuTop = 0;
  Serial.println("[SCAN] Enumerando /NES sem abrir cada arquivo...");
  File root=SD.open("/NES",FILE_READ);
  if(!root || !root.isDirectory()) {
    Serial.println("[SCAN][FATAL] Pasta /NES nao existe ou nao e diretorio");
    if(root)root.close();
    sdCatalogVerified=false;
    return false;
  }
  sdCatalogVerified=true;
  const uint32_t t0=millis();
  uint32_t entries=0, skipped=0;
  while(gameCount < MAX_GAMES) {
    bool isDir=false;
    String fpath=root.getNextFileName(&isDir);
    if(fpath.length()==0) break;
    ++entries;
    if(isDir){ ++skipped; continue; }
    const char *path=fpath.c_str();
    const char *name=strrchr(path,'/');
    name = name ? name+1 : path;
    if(!isNesName(name) || fpath.length() >= PATH_CAP){++skipped;continue;}
    NesGame &entry=gameList[gameCount];
    snprintf(entry.path,sizeof(entry.path),"%s",path);
    const size_t len=strlen(name)-4;
    snprintf(entry.title,sizeof(entry.title),"%.*s",(int) (len < NAME_CAP-1 ? len : NAME_CAP-1),name);
    ++gameCount;
    if(gameCount<=5 || gameCount%25==0)
      Serial.printf("[SCAN] #%u %s\n",(unsigned)gameCount,entry.path);
    if((entries&15)==0){delay(1);}
  }
  root.close();
  // Ordenacao de no maximo 500 registros pequenos: so depois de fechar pasta.
  for(uint16_t i=1;i<gameCount;i++) {
    NesGame tmp=gameList[i];
    int j=i;
    while(j>0 && strcasecmp(gameList[j-1].title,tmp.title)>0){gameList[j]=gameList[j-1];--j;}
    gameList[j]=tmp;
  }
  Serial.printf("[SCAN][DONE] encontrados=%u inspecionados=%u ignorados=%u elapsed=%lu ms%s\n",
    (unsigned)gameCount,(unsigned)entries,(unsigned)skipped,(unsigned long)(millis()-t0),
    gameCount == MAX_GAMES ? " LIMIT=500" : "");
  return true;
}

static bool searchMatches(const char*title){
  if(!librarySearch[0])return true;
  for(const char *p=title;*p;p++){
    unsigned i=0;
    while(librarySearch[i] && p[i] && toupper((unsigned char)p[i])==toupper((unsigned char)librarySearch[i]))i++;
    if(!librarySearch[i])return true;
  }
  return false;
}
static int gameIndexById(uint32_t id){
  for(unsigned i=0;i<gameCount;i++)if(HaloLibrary::gameId(gameList[i].path)==id)return (int)i;
  return -1;
}
static void buildVisible(){
  uint32_t old=0;
  if(visibleCount && visibleSelection<visibleCount)old=HaloLibrary::gameId(gameList[visibleGames[visibleSelection]].path);
  visibleCount=0;visibleSelection=0;menuTop=0;
  if(libraryView==2){
    for(unsigned r=0;r<HaloLibrary::recentCount();r++){
      int i=gameIndexById(HaloLibrary::recent(r));
      if(i>=0 && searchMatches(gameList[i].title))visibleGames[visibleCount++]=(uint16_t)i;
    }
  }else{
    for(uint16_t i=0;i<gameCount;i++){
      if(libraryView==1&&!HaloLibrary::isFavorite(HaloLibrary::gameId(gameList[i].path)))continue;
      if(!searchMatches(gameList[i].title))continue;
      visibleGames[visibleCount++]=i;
    }
  }
  for(unsigned i=0;i<visibleCount;i++)if(HaloLibrary::gameId(gameList[visibleGames[i]].path)==old){visibleSelection=i;break;}
  Serial.printf("[R12][LIB][FILTER] tipo=%u filtro=%s jogos=%u\n",(unsigned)libraryView,librarySearch,(unsigned)visibleCount);
}
static void drawKeyboard(){
  tft.fillScreen(0x000B);
  tft.fillRect(0,0,320,29,TFT_NAVY);
  tft.setTextSize(2);tft.setTextColor(TFT_CYAN,TFT_NAVY);tft.setCursor(6,7);tft.print("BUSCAR ROM");
  tft.fillRoundRect(6,36,308,30,3,0x2104);
  tft.setTextColor(TFT_WHITE,0x2104);tft.setTextSize(2);tft.setCursor(15,43);tft.printf("%-12.12s",librarySearch);
  for(unsigned i=0;i<28;i++){
    unsigned x=8+(i%7)*44,y=72+(i/7)*32;
    const bool hot=i==keyboardCursor;
    const uint16_t bg=hot?0x03FF:0x18C3;
    tft.fillRoundRect(x,y,39,29,4,bg);
    tft.setTextColor(hot?TFT_BLACK:TFT_WHITE,bg);tft.setTextSize(i==27?1:2);
    tft.setCursor(x+(i==27?7:12),y+(i==27?9:6));
    if(i<26)tft.printf("%c",(char)('A'+i));else tft.print(i==26?"_":"OK");
  }
  tft.setTextSize(1);tft.setTextColor(TFT_LIGHTGREY,0x000B);tft.setCursor(6,213);
  tft.print("X: inserir | []: apagar | TRI: sair");
}
static void drawResumeConfirm(){
  tft.fillScreen(0x000B);
  tft.setTextSize(2);tft.setTextColor(TFT_CYAN,0x000B);tft.setCursor(22,38);tft.print("CONTINUAR JOGO?");
  int last=gameIndexById(HaloLibrary::lastGame());
  tft.setTextSize(1);tft.setTextColor(TFT_WHITE,0x000B);tft.setCursor(10,88);
  if(last>=0)tft.printf("%.42s",gameList[last].title);
  tft.setCursor(18,147);tft.setTextSize(2);tft.setTextColor(TFT_GREEN,0x000B);tft.print("X = SIM");
  tft.setCursor(18,177);tft.setTextColor(TFT_YELLOW,0x000B);tft.print("B = VOLTAR");
}
static void drawMenu(){
  if(keyboardOpen){drawKeyboard();return;}
  if(resumePrompt){drawResumeConfirm();return;}
  tft.fillScreen(0x000B);
  // Cabecalho compacto - 320x240, colunas esquerda (190px) e direita (126px)
  tft.fillRect(0,0,320,28,TFT_NAVY);
  tft.setTextSize(2);tft.setTextColor(TFT_CYAN,TFT_NAVY);
  tft.setCursor(8,5);tft.print("NES-S3");
  tft.setTextSize(1);tft.setTextColor(TFT_WHITE,TFT_NAVY);
  tft.setCursor(252,10);tft.printf("%u ROMS",(unsigned)gameCount);
  if(!sdCatalogVerified){
    tft.setTextColor(TFT_RED,0x000B);tft.setCursor(9,101);
    tft.print("SD OFFLINE - VERIFIQUE OS FIOS");return;
  }
  const int last=gameIndexById(HaloLibrary::lastGame());
  const uint16_t bar=last>=0?0x01D5:0x1082;
  tft.fillRoundRect(4,32,312,27,4,bar);
  tft.setTextSize(1);tft.setTextColor(TFT_WHITE,bar);tft.setCursor(9,42);
  if(last>=0){
    tft.print("START: CONTINUAR ");
    char lastName[23];snprintf(lastName,sizeof(lastName),"%.22s",gameList[last].title);
    tft.print(lastName);
  }else tft.print("START: CONTINUAR - SEM JOGO RECENTE");
  tft.setCursor(7,67);tft.setTextColor(TFT_YELLOW,0x000B);
  tft.printf("<%s>",libraryView==0?"TODOS":libraryView==1?"FAVORITOS":"RECENTES");
  tft.drawFastVLine(194,80,138,0x5AEB);
  if(visibleSelection<menuTop)menuTop=visibleSelection;
  if(visibleSelection>=menuTop+MENU_ROWS)menuTop=visibleSelection-MENU_ROWS+1;
  if(!visibleCount){
    tft.setTextColor(TFT_ORANGE,0x000B);
    tft.setCursor(7,105);tft.print("NENHUM JOGO");
  }
  for(unsigned row=0;row<MENU_ROWS;row++){
    const unsigned vi=menuTop+row;
    if(vi>=visibleCount)break;
    const uint16_t gi=visibleGames[vi];
    const bool focus=vi==visibleSelection;
    const bool fav=HaloLibrary::isFavorite(HaloLibrary::gameId(gameList[gi].path));
    const uint16_t bg=focus?0x03B9:0x000B;
    const int y=84+(int)row*18;
    tft.fillRoundRect(3,y-2,187,17,3,bg);
    tft.setTextSize(1);tft.setTextColor(focus?TFT_WHITE:TFT_LIGHTGREY,bg);
    tft.setCursor(7,y+2);tft.printf("%c%c",focus?'>':' ',fav?'*':' ');
    // 26 caracteres no maximo e reticencias reais quando truncado.
    char shortTitle[27];
    const char *name=gameList[gi].title;
    if(strlen(name)>26){memcpy(shortTitle,name,23);memcpy(shortTitle+23,"...",4);}
    else snprintf(shortTitle,sizeof(shortTitle),"%s",name);
    tft.print(shortTitle);
  }
  // Painel de capa: abre o SD apenas quando o jogo selecionado muda.
  tft.fillRoundRect(199,76,117,142,5,0x1082);
  if(visibleCount){
    const uint16_t gi=visibleGames[visibleSelection];
    const bool cover= NesS3Covers::draw(tft,gameList[gi].path,205,79);
    if(!cover) NesS3Covers::drawMissing(tft,205,79);
  }else NesS3Covers::drawMissing(tft,205,79);
  tft.drawFastHLine(0,220,320,TFT_DARKGREY);
  tft.setTextSize(1);tft.setTextColor(TFT_YELLOW,0x000B);
  tft.setCursor(5,225);tft.print("X:JOGAR  []:FAV  TRI:BUSCAR  <>:FILTRO");
}

// Teste isolado de fopen/fread do ESP-IDF. Nofrendo usa stdio via FatFS.
// Nao presumir que cardSize/scan inicial garante leituras futuras.
// Importante: NAO faz recovery para ENOENT (arquivo ausente) ou ROM invalida.
static bool validateSelectedRom(const char *sdPath){
 __atomic_store_n(&nesExpectedPayloadReady,false,__ATOMIC_RELEASE);
 __atomic_store_n(&nesPayloadValidationCalls,0,__ATOMIC_RELEASE);
 char sdFile[PATH_CAP+8];
 int n=snprintf(sdFile,sizeof(sdFile),"/sd%s",sdPath);
 if(n<=0||(size_t)n>=sizeof(sdFile))return false;
 if(!HaloRomCache::install()){Serial.println("[R9][ROMVFS][FAIL] indisponivel");return false;}
 for(int attempt=0;attempt<2;attempt++){
   uint32_t hash=0,ms=0,done=0;int err=0;
   Serial.printf("[R9][ROM][TRY] %d/2 src=%s\n",attempt+1,sdFile);
   if(HaloRomCache::load(sdFile,&hash,&ms,&err,&done)){
     __atomic_store_n(&nesExpectedPayloadHash,hash,__ATOMIC_RELEASE);
     __atomic_store_n(&nesExpectedPayloadReady,true,__ATOMIC_RELEASE);
     lastRomHash=hash;
     sdReadMsLast=ms;
     snprintf(selectedStdIoPath,sizeof(selectedStdIoPath),"%s",HaloRomCache::path());
     Serial.printf("[R9][ROM][OK] SD-ONCE PSRAM=%u hash=%08lX readMs=%lu Nofrendo=%s\n",
        (unsigned)HaloRomCache::size(),(unsigned long)hash,(unsigned long)ms,selectedStdIoPath);
     return true;
   }
   Serial.printf("[R9][ROM][FAIL] read=%lu errno=%d\n",(unsigned long)done,err);
   if(attempt==0 && sdErrorIsDeviceFault(err) && recoverSDOnMenu(err))continue;
   break;
 }
 return false;
}

static void emulatorThread(void *argument){
  const char *path=(const char *)argument;
  char *argv[1]={(char *)path};
  Serial.printf("[EMU] nofrendo_main com arquivo=%s\n",path);
  Serial.flush(); // Manter marcador legivel antes de possivel panic.
  reportMemory("before nofrendo");
  int status=nofrendo_main(1,argv);
  const uint32_t checked=__atomic_load_n(&nesPayloadValidationCalls,__ATOMIC_ACQUIRE);
  Serial.printf("[ROM][PATCH] validacao de bancos pelo Nofrendo: %s calls=%lu\n",
                checked?"PRESENTE":"AUSENTE - aplique patch v0.2.9.3",(unsigned long)checked);
  Serial.printf("[EMU][RETURN] Nofrendo retornou status=%d\n",status);
  reportMemory("after nofrendo");
  Serial.println("[EMU][DIAG] Sinalizando retorno cooperativo ao menu, sem ESP.restart");
  Serial.flush();
  // A tarefa de emulacao nao usa o TFT nem desaloca estruturas do menu.
  // So informa ao loop principal (Core Arduino) que o nucleo terminou.
  __atomic_store_n(&emulationCompleted,true,__ATOMIC_RELEASE);
  vTaskDelete(nullptr);
}

static void startGame(bool resumeAuto=false) {
  if(!gameList || !gameCount || menuSelection>=gameCount || isEmulating)return;
  // Apos recover falhar, SD.end() foi executado: novos fopen retornariam ENOENT.
  // Tratar SD offline como falha de dispositivo, nao como ROM desaparecida.
  if(!sdOnline) {
    Serial.println("[SD][MENU][OFFLINE] tentando restabelecer acesso antes de abrir ROM");
    if(!recoverSDOnMenu(ENODEV)) {
      screenMessage("Cartao SD indisponivel", "Confira SD e conexoes");
      delay(1200);drawMenu();return;
    }
  }
  const char *rom=gameList[menuSelection].path;
  Serial.printf("[MENU][START] %s\n",rom);
  drawHaloBoot();
  if(!validateSelectedRom(rom)){
    screenMessage("Falha ao ler a ROM", "Verifique serial e SD");delay(1500);drawMenu();return;
  }
  // Nome deterministico por caminho original e hash real da ROM.
  // Deve ser configurado antes de iniciar a tarefa Nofrendo.
  HaloSave::selectGame(rom,lastRomHash);
  HaloLibrary::opened(HaloLibrary::gameId(rom));
  launchResumeAuto=resumeAuto;
  Serial.printf("[R12][RESUME] requested=%u last=%08lX\n",(unsigned)resumeAuto,(unsigned long)HaloLibrary::lastGame());
  // O Nofrendo ocupa apenas x=32..287. Sem essa limpeza, sobram partes
  // do menu e do titulo nas duas barras laterais durante o jogo inteiro.
  Serial.println("[R12][SPLASH] mantida ate primeiro quadro significativo NES");
  __atomic_store_n(&gamepadMask,0,__ATOMIC_RELEASE);
  __atomic_store_n(&pauseMenuRequested,false,__ATOMIC_RELEASE);
  __atomic_store_n(&pauseMenuActive,false,__ATOMIC_RELEASE);
  __atomic_store_n(&pauseMenuButtons,(uint16_t)0,__ATOMIC_RELEASE);
  __atomic_store_n(&pauseWaitForRelease,false,__ATOMIC_RELEASE);
  __atomic_store_n(&exitRequested,false,__ATOMIC_RELEASE);
  __atomic_store_n(&emulationCompleted,false,__ATOMIC_RELEASE);
  isEmulating=true;
  __atomic_store_n(&producedFrames,0,__ATOMIC_RELAXED);
  __atomic_store_n(&emulatorYields,0,__ATOMIC_RELAXED);
  __atomic_store_n(&emulatorStackMinFree,UINT32_MAX,__ATOMIC_RELAXED);
  __atomic_store_n(&videoTransferLastUs,0,__ATOMIC_RELAXED);
  __atomic_store_n(&videoTransferMaxUs,0,__ATOMIC_RELAXED);
  __atomic_store_n(&videoSamplesMade,0,__ATOMIC_RELAXED);
  __atomic_store_n(&videoSampleNonzeroPercent,0,__ATOMIC_RELAXED);
  __atomic_store_n(&videoSampleUniqueColors,0,__ATOMIC_RELAXED);
  __atomic_store_n(&videoSampleHash,0,__ATOMIC_RELAXED);
  perfPreviousAt=millis();
  perfPreviousFrames=0;
  lastPauseResumeEpoch=__atomic_load_n(&pauseResumeEpoch,__ATOMIC_ACQUIRE);
  BaseType_t created=xTaskCreatePinnedToCore(emulatorThread,"NES_EMU",32768,(void *)selectedStdIoPath,1,nullptr,0);
  if(created!=pdPASS){
    Serial.println("[EMU][FAIL] criacao da tarefa de emulacao falhou");
    // Sem tarefa NES, a ROM em PSRAM nao tem consumidor e precisa ser liberada.
    HaloRomCache::release();
    __atomic_store_n(&nesExpectedPayloadReady,false,__ATOMIC_RELEASE);
    isEmulating=false;drawMenu();
  }
}

void setup(){
  Serial.begin(115200);delay(700);
  Serial.println("\n===============================================\n NES-S3 v0.2.9.11-r13.1-demo-rc2 -- SEM WIFI\n===============================================");
  Serial.printf("[BOOT] CPU=%u MHz flash=%u PSRAM=%u\n",ESP.getCpuFreqMHz(),ESP.getFlashChipSize(),ESP.getPsramSize());
  Serial.printf("[RESET] esp_reset_reason=%d (consulte esp_system.h para descricao exata)\n", (int)esp_reset_reason());
  reportMemory("boot");
  // SD deve entrar no modo SPI ANTES de qualquer comando enviado ao TFT.
  // Referencia: Espressif, sdspi_share (Inicializacao do barramento compartilhado).
  // TFT e SD usam a MESMA instancia HSPI do TFT_eSPI, nao um novo SPIClass.
  pinMode(TFT_CS,OUTPUT);pinMode(SD_CS_PIN,OUTPUT);
  digitalWrite(TFT_CS,HIGH);digitalWrite(SD_CS_PIN,HIGH);
  // Margem de estabilizacao do cartao ao ligar. Sem alterar o clock de 1 MHz
  // nem a ordem SD-FIRST; nao substitui medicao eletrica se houver falha.
  delay(120);
  Serial.println("[R13.1][SD][POWER_SETTLE] 120ms com TFT_CS=HIGH SD_CS=HIGH");
  SPIClass &sharedSPI=tft.getSPIinstance();
  Serial.println("[SPI][BOOTORDER] SD_FIRST: iniciando HSPI 12/13/11 antes de qualquer transacao TFT");
  if(!sharedSPI.begin(TFT_SCLK,TFT_MISO,TFT_MOSI,-1)) {
    Serial.println("[SPI][FATAL] spi.begin falhou (pinos/barramento)");
    while(true)delay(1000);
  }
  // O SD Arduino realiza a sequencia de clocks/CMD0 a baixa velocidade.
  // Nao enviar bytes de tela antes de SD.begin: alguns cartoes iniciam em modo SD nativo.
  Serial.println("[SD] SD.begin(CS=14, TFT SPI HSPI, 1MHz, mount=/sd) [PRE-TFT]");
  const bool sdReady=mountSD(SD_BOOT_RETRIES,"MOUNT");
  if(!sdReady) {
    Serial.println("[SD][FATAL] Falha apos 3 tentativas PRE-TFT; conferir CS/MISO/3V3, tentar power cycle completo");
    // Apenas AGORA inicializar display para apresentar erro; nao tentar SD de novo
    // depois de enviar comandos TFT num cartao que pode continuar em modo SD nativo.
    Serial.println("[TFT] tft.init() apos falha SD para mensagem de erro");
    tft.init();tft.setRotation(TFT_ROTATION);applyColorPipeline("SD_FAIL");
    screenMessage("ERRO SD", "CS14/MISO/3V3 - desligue e religue");
    while(true)delay(1000);
  }
  const bool directoryBeforeTFT=probeNESDirectory("PRE-TFT");
  Serial.printf("[SD] mounted card=%lluMB sdClock=%lu kHz (SD-first)\n",
    (unsigned long long)(SD.cardSize()/1048576ULL),(unsigned long)(SD_SAFE_FREQ_HZ/1000));
  // spi.begin() dentro do TFT_eSPI detecta que a instancia ja foi iniciada.
  Serial.println("[TFT] tft.init() SOMENTE depois de SD em modo SPI");
  tft.init();tft.setRotation(TFT_ROTATION);applyColorPipeline("BOOT");
  Serial.printf("[TFT] rotation=%u width=%d height=%d\n",TFT_ROTATION,tft.width(),tft.height());
  if(tft.width()!=320 || tft.height()!=240) {
    Serial.println("[TFT][FATAL] Resolucao incorreta: esperava 320x240 (landscape)");
    while(true)delay(1000);
  }
  drawHaloBoot();
  gameList=(NesGame *)heap_caps_calloc(MAX_GAMES,sizeof(NesGame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!gameList){Serial.println("[PSRAM][FAIL] indice nao alocado");screenMessage("ERRO PSRAM");while(true)delay(1000);}
  bool directoryAfterTFT=probeNESDirectory("POST-TFT");
  Serial.printf("[SD][TRANSITION] PRE-TFT=%s POST-TFT=%s\n",
    directoryBeforeTFT?"OK":"FAIL",directoryAfterTFT?"OK":"FAIL");
  bool catalogOk=directoryAfterTFT && scanGameNames();
  // Este teste e intencional: exercita TFT->SD como o menu e as ROMs.
  tft.fillRect(0,180,320,5,TFT_BLUE);
  bool spiCheck=probeNESDirectory("AFTER-TFT-DRAW");
  Serial.printf("[SPI][TFT=>SD] /NES = %s\n",spiCheck?"OK":"FAIL");
  if(!catalogOk || !spiCheck) {
    // v0.2.9.1 nao recuperava o caso: SD.begin OK, mas /NES inacessivel.
    Serial.println("[SD][BOOT-RECOVER] FAT montado mas pasta inacessivel; desmontando e remontando sem reset");
    gameCount=0;
    sdCatalogVerified=false;
    if(recoverSDOnMenu(EIO)){
      catalogOk=scanGameNames();
      spiCheck=probeNESDirectory("FINAL");
      sdCatalogVerified=catalogOk && spiCheck;
    }else{
      catalogOk=false;
      sdCatalogVerified=false;
    }
    if(!catalogOk || !spiCheck){
      gameCount=0;
      sdCatalogVerified=false;
      Serial.println("[SD][BOOT-RECOVER][FAIL] nao apresentar lista potencialmente obsoleta");
    }else Serial.printf("[SD][BOOT-RECOVER][OK] jogos=%u\n",(unsigned)gameCount);
  }
  HaloLibrary::load();
  buildVisible();
  HaloAudio::setVolume(HaloLibrary::volume());
  HaloAudio::begin();
  NesS3Covers::begin();
  Serial.println("[BLE] Dabble.begin(ESP32-S3-NES-LAB)");
  Dabble.begin("ESP32-S3-NES-LAB");
  reportMemory("ready");
  Serial.println("[READY] NES-S3 R13.1 DEMO RC2: offline, biblioteca, capas SD, SNSS e I2S.");
  Serial.println("[CORE] Nofrendo v0.2.4 + patches v0.2.9.3/v0.2.9.4 (ROM integrity + WDT).");
  Serial.println("[SD][POLICY] recovery apenas no MENU apos EIO/ENODEV/ENXIO, sem segundo SPI.");
  Serial.println("[VIDEO][PERF] blitLastMs/blitMaxMs: medem tempo de escrita SPI; nao medem VSync/TE.");
  Serial.println("[R9.1][COLOR][LOCKED] R8_EXATA: ANEMOIA CEU AZUL; sem tons quentes R9; paleta unica");
  Serial.println("[R9][VFS] ROM carregada uma vez em PSRAM; Nofrendo sem acesso posterior a SD");
  Serial.println("[PAUSE] HOLD START+SELECT 700ms -> CONTINUAR / CONFIGURACOES / SAIR DO JOGO");
  Serial.println("[R11][STATES] /save; slots 0-3 + AUTO 9; gravacao a cada 120s e ao SAIR");
  Serial.println("[R13][MENU] coluna esquerda ROMs, direita CAPAS /covers/*.cov; <> filtros, TRI busca, [] fav, START continuar");
  drawMenu();
}

void loop(){
  static uint32_t comboSince=0;
  static bool comboConsumed=false;
  Dabble.processInput();
  uint16_t buttons=readDabbleButtons();
  if(isEmulating){
    // O proprio Nofrendo libera CPU, memoria e timer antes de sinalizar fim.
    // Essa transicao e feita no loop principal, nunca dentro da tarefa NES.
    if(__atomic_load_n(&emulationCompleted,__ATOMIC_ACQUIRE)){
      __atomic_store_n(&gamepadMask,0,__ATOMIC_RELEASE);
      __atomic_store_n(&emulationCompleted,false,__ATOMIC_RELEASE);
      __atomic_store_n(&exitRequested,false,__ATOMIC_RELEASE);
      HaloAudio::end();
      HaloRomCache::release();
      __atomic_store_n(&nesExpectedPayloadReady,false,__ATOMIC_RELEASE);
      buildVisible();
      Serial.println("[R9][ROMVFS][RELEASE] PSRAM liberada apos emulacao");
      isEmulating=false;
      comboSince=0;comboConsumed=false;
      __atomic_store_n(&pauseMenuRequested,false,__ATOMIC_RELEASE);
      __atomic_store_n(&pauseMenuActive,false,__ATOMIC_RELEASE);
      __atomic_store_n(&pauseMenuButtons,(uint16_t)0,__ATOMIC_RELEASE);
      __atomic_store_n(&pauseWaitForRelease,false,__ATOMIC_RELEASE);
      Serial.println("[MENU][RESTORE] Nofrendo finalizado; voltando sem ESP.restart");
      reportMemory("after menu return");
      drawMenu();
      delay(400); // evitar que START+SELECT vire um novo comando no menu
      return;
    }
    // Menu de pausa: controle continua sendo lido via BLE no loop principal,
    // mas nenhuma tecla e encaminhada ao NES enquanto o menu estiver ativo.
    __atomic_store_n(&pauseMenuButtons,buttons,__ATOMIC_RELEASE);
    if(__atomic_load_n(&pauseWaitForRelease,__ATOMIC_ACQUIRE)) {
      if(buttons==0) __atomic_store_n(&pauseWaitForRelease,false,__ATOMIC_RELEASE);
      buttons=0;
      __atomic_store_n(&gamepadMask,(uint16_t)0,__ATOMIC_RELEASE);
      delay(5);return;
    }
    if(__atomic_load_n(&pauseMenuRequested,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&pauseMenuActive,__ATOMIC_ACQUIRE)) {
      __atomic_store_n(&gamepadMask,(uint16_t)0,__ATOMIC_RELEASE);
      delay(5);return;
    }
    // Detectar chord antes de enviar Start/Select ao jogo. Pressionar ambos
    // nao dispara o menu ate completar o tempo minimo de seguranca.
    if((buttons&0x0C)==0x0C) {
      if(!comboSince)comboSince=millis();
      if(!comboConsumed && (uint32_t)(millis()-comboSince)>=700u) {
        comboConsumed=true;
        __atomic_store_n(&pauseMenuRequested,true,__ATOMIC_RELEASE);
        __atomic_store_n(&gamepadMask,(uint16_t)0,__ATOMIC_RELEASE);
        Serial.println("[PAUSE][REQUEST] START+SELECT: abrindo menu sem encerrar ROM");
      }
      buttons=(uint16_t)(buttons&~0x0CU);
    } else {comboSince=0;comboConsumed=false;}
    if(__atomic_load_n(&pauseMenuRequested,__ATOMIC_ACQUIRE)) {
      delay(5);return;
    }
    // SELECT+B+direcao e controle normal do jogo; hotkey tecnico removido.
    __atomic_store_n(&gamepadMask,buttons,__ATOMIC_RELEASE);
    // O tempo parado no menu nao deve derrubar artificialmente o FPS.
    // Ignora somente a janela atual; a contagem de frames NES continua intacta.
    const uint32_t resumedEpoch=__atomic_load_n(&pauseResumeEpoch,__ATOMIC_ACQUIRE);
    if(resumedEpoch!=lastPauseResumeEpoch){
      lastPauseResumeEpoch=resumedEpoch;
      perfPreviousAt=millis();
      perfPreviousFrames=__atomic_load_n(&producedFrames,__ATOMIC_ACQUIRE);
      Serial.println("[PERF][PAUSE] janela de FPS reiniciada apos menu");
    }
    if(millis()-perfPreviousAt>=5000){
      uint32_t now=millis(), frames=__atomic_load_n(&producedFrames,__ATOMIC_ACQUIRE);
      if(perfPreviousAt) {
        const uint32_t yields=__atomic_load_n(&emulatorYields,__ATOMIC_RELAXED);
        const uint32_t stackLow=__atomic_load_n(&emulatorStackMinFree,__ATOMIC_RELAXED);
        const uint32_t lastUs=__atomic_load_n(&videoTransferLastUs,__ATOMIC_RELAXED);
        const uint32_t worstUs=__atomic_load_n(&videoTransferMaxUs,__ATOMIC_RELAXED);
        Serial.printf("[PERF] frames=%lu displayedFPS=%.1f heap=%u psram=%u yields=%lu stackMinFree=%s%lu blitLastMs=%.2f blitMaxMs=%.2f\n",
          (unsigned long)frames,(frames-perfPreviousFrames)*1000.0f/(now-perfPreviousAt),ESP.getFreeHeap(),ESP.getFreePsram(),
          (unsigned long)yields,stackLow==UINT32_MAX?"? ":"",(unsigned long)(stackLow==UINT32_MAX?0:stackLow),
          lastUs/1000.0f,worstUs/1000.0f);
        Serial.printf("[VIDEO][SAMPLE] samples=%lu nonZeroPct=%lu unique=%lu hash=0x%08lX romReadHash=0x%08lX banksChecked=%lu\n",
          (unsigned long)__atomic_load_n(&videoSamplesMade,__ATOMIC_RELAXED),
          (unsigned long)__atomic_load_n(&videoSampleNonzeroPercent,__ATOMIC_RELAXED),
          (unsigned long)__atomic_load_n(&videoSampleUniqueColors,__ATOMIC_RELAXED),
          (unsigned long)__atomic_load_n(&videoSampleHash,__ATOMIC_RELAXED),
          (unsigned long)lastRomHash,
          (unsigned long)__atomic_load_n(&nesPayloadValidationCalls,__ATOMIC_RELAXED));
        if(frames>0 && __atomic_load_n(&nesPayloadValidationCalls,__ATOMIC_RELAXED)==0)
          Serial.println("[ROM][PATCH][WARN] bancos nao validados pelo patch 0.2.9.3; carregamento pode estar incompleto");
        if(frames>0 && HaloAudio::framesProduced()==0 && HaloAudio::enabled())
          Serial.println("[R12][AUDIO][WARN] hook da APU ausente; execute CORRIGIR_NOFRENDO_R12_1.bat e recompile");
        if(HaloAudio::framesProduced()>0)
          Serial.printf("[R12][AUDIO][PERF] frames=%lu dropped=%lu volume=%d\n",(unsigned long)HaloAudio::framesProduced(),(unsigned long)HaloAudio::framesDropped(),HaloAudio::volume());
      }
      perfPreviousAt=now;perfPreviousFrames=frames;
    }
    delay(5);return;
  }
  static uint16_t prev=0;
  static bool prevCross=false,prevSquare=false,prevTriangle=false,prevStart=false,prevCircle=false;
  const bool riseUp=(buttons&0x10)&&!(prev&0x10);
  const bool riseDown=(buttons&0x20)&&!(prev&0x20);
  const bool riseLeft=(buttons&0x40)&&!(prev&0x40);
  const bool riseRight=(buttons&0x80)&&!(prev&0x80);
  const bool cross=GamePad.isCrossPressed();
  const bool square=GamePad.isSquarePressed();
  const bool triangle=GamePad.isTrianglePressed();
  const bool circle=GamePad.isCirclePressed();
  const bool start=GamePad.isStartPressed();
  const bool pressX=cross&&!prevCross,pressSquare=square&&!prevSquare;
  const bool pressTriangle=triangle&&!prevTriangle,pressCircle=circle&&!prevCircle;
  const bool pressStart=start&&!prevStart;
  // A navegacao principal nao interfere nos comandos B/SELECT dentro do jogo.
  if(resumePrompt){
    if(pressX){
      const int idx=gameIndexById(HaloLibrary::lastGame());
      resumePrompt=false;
      if(idx>=0){menuSelection=(uint16_t)idx;startGame(true);}else drawMenu();
    }else if(pressSquare||pressCircle||pressTriangle){resumePrompt=false;drawMenu();}
  }else if(keyboardOpen){
    if(riseLeft){keyboardCursor=(uint8_t)((keyboardCursor/7)*7+(keyboardCursor%7+6)%7);drawMenu();}
    if(riseRight){keyboardCursor=(uint8_t)((keyboardCursor/7)*7+(keyboardCursor%7+1)%7);drawMenu();}
    if(riseUp){keyboardCursor=(uint8_t)((keyboardCursor+21)%28);drawMenu();}
    if(riseDown){keyboardCursor=(uint8_t)((keyboardCursor+7)%28);drawMenu();}
    if(pressSquare){size_t n=strlen(librarySearch);if(n)librarySearch[n-1]=0;buildVisible();drawMenu();}
    if(pressCircle){librarySearch[0]=0;buildVisible();drawMenu();}
    if(pressTriangle){keyboardOpen=false;drawMenu();}
    if(pressX){
      if(keyboardCursor==27){keyboardOpen=false;buildVisible();drawMenu();}
      else {size_t n=strlen(librarySearch);if(n<sizeof(librarySearch)-1){librarySearch[n]=keyboardCursor==26?' ':(char)('A'+keyboardCursor);librarySearch[n+1]=0;buildVisible();}drawMenu();}
    }
  }else if(gameCount){
    if(riseLeft){libraryView=(uint8_t)((libraryView+2)%3);buildVisible();drawMenu();}
    if(riseRight){libraryView=(uint8_t)((libraryView+1)%3);buildVisible();drawMenu();}
    if(riseUp&&visibleCount){visibleSelection=(uint16_t)((visibleSelection+visibleCount-1)%visibleCount);drawMenu();}
    if(riseDown&&visibleCount){visibleSelection=(uint16_t)((visibleSelection+1)%visibleCount);drawMenu();}
    if(pressSquare&&visibleCount){HaloLibrary::toggleFavorite(HaloLibrary::gameId(gameList[visibleGames[visibleSelection]].path));buildVisible();drawMenu();}
    if(pressTriangle){keyboardOpen=true;drawMenu();}
    if(pressStart && gameIndexById(HaloLibrary::lastGame())>=0){resumePrompt=true;drawMenu();}
    if(pressX&&visibleCount){menuSelection=visibleGames[visibleSelection];startGame(false);}
  }
  prevCross=cross;prevSquare=square;prevTriangle=triangle;prevCircle=circle;prevStart=start;
  prev=buttons;
  delay(8);
}
