# Relatório de auditoria do código — R13.1 Demo RC2

**23/10/2026 | Resultado: candidata a pré-release, não validada em hardware.**

## Origem e comparação byte a byte

- Base: firmware da R13.1 Demo RC1 recebido neste histórico e backup R13 anterior.
- São **15 arquivos do sketch Arduino**. Comparação da RC2 com RC1:
  - Modificados: `NES_S3.ino` (identificação), `halo_savestates.cpp`, `halo_library.cpp`.
  - Os outros **12 arquivos são idênticos byte a byte** à RC1. Em especial `osd_port.cpp`, vídeo, paleta, áudio, capas, VFS e APIs de controle estão preservados.
- Não foi adicionada nenhuma dependência de Wi-Fi/HTTP, nem distribuído catálogo, ROM comercial, binário ESP32 ou biblioteca Nofrendo externa.

## Correções efetivas RC2

**HALO SAVE**
- Antes: `filePresent()` colapsava `stat()` com `EIO` e `ENOENT`. Isso podia classificar como ausente um arquivo existente inacessível, prosseguindo para `rename(tmp,file)`.
- Agora: `stat(file)` é checado explicitamente e o commit é cancelado em erro distinto de `ENOENT`.
- Não sobrescreve principal inválido sem backup/temporário recuperado. Protege um save corrompido até inspeção no PC.
- A limpeza de `_TMP` falha fechada em erro de E/S; evita tentativas destrutivas repetidas.

**HALO LIBRARY**
- Antes: `remove(BACK)` podia eliminar o único backup útil mesmo com principal corrompido, e `stat(PATH)` com erro era confundido com ausência. `parse(TEMP)` também regravava estruturas na RAM durante mera validação.
- Agora: validação opcional sem mutar estado; antes de rotacionar backup, exige `PATH` regular e com checksum válido, e trata erro do `stat`/`remove` como aborto sem rotação. O destino novo só é promovido após verificação do TMP.
- Consequência intencional: índice principal corrompido deixa os dados em RAM acessíveis quando o BACKUP é lido, mas **bloqueia nova persistência** até reparação manual segura no computador; este é um compromisso deliberado para não perder o backup válido.

## Inspeção dos 15 arquivos

| Arquivo | Status RC2 | Observações |
|---|---|---|
| `NES_S3.ino` | Identificação RC2 | Setup SD-FIRST e SPI HSPI, índices/menus/controles sem alterações |
| `halo_savestates.cpp` | Alterado + testado | Recovery SNSS, gravação e proteção de primário inválido; escrita BLE ainda não validada |
| `halo_savestates.h` | Idêntico | Interface SNSS |
| `halo_library.cpp` | Alterado + testado | FNV metadata, rotação e proteção de BACKUP |
| `halo_library.h` | Idêntico | Interface de favoritos/recentes/volume |
| `rom_cache_vfs.cpp` | Idêntico | ROM PSRAM + iNES + limite 4 MiB; exige Nofrendo patched externo |
| `rom_cache_vfs.h` | Idêntico | Interface cache |
| `osd_port.cpp` | Idêntico | OSD Nofrendo, PPU, timer, pausa, vídeo; comparação estática, não compilado neste ambiente |
| `halo_audio.cpp` | Idêntico | I2S 22020, fila e áudio; sem ensaio físico |
| `halo_audio.h` | Idêntico | Interface áudio |
| `nes_s3_covers.cpp` | Idêntico | Capa NS3C, cache PSRAM, sem alteração visual |
| `nes_s3_covers.h` | Idêntico | Dimensões da capa |
| `halo_fixed_palette.h` | Idêntico | Paleta da R8 e índice `$31`, sem recalcular valores |
| `nes_color.h` | Idêntico | Conversão RGB565 |
| `LICENSE_GPL3.txt` | Idêntico | Texto GPL3 do pacote, não substitui revisão de proveniência |

## Testes executados de verdade

1. `g++ -std=c++17 -Wall -Wextra -Werror` compilou os arquivos reais `halo_savestates.cpp` e `halo_library.cpp` contra stubs mínimos de ambiente Arduino/Nofrendo; **13 cenários passaram**.
2. Os mesmos módulos e testes passaram novamente com `-fsanitize=address,undefined`, sem erro de sanitizer no ensaio.
3. Checagem de nomes, ausência de `#include <WiFi.h>`/HTTPClient/LittleFS nos arquivos de firmware e SHA-256.
4. Comparação byte a byte RC1/RC2 comprovou 12 arquivos inalterados.

**Não executado:** Arduino IDE/arduino-cli para alvo ESP32-S3 (toolchain e Nofrendo patched indisponíveis), upload, áudio real, power cycle, leitura/escrita no FAT com BLE, FPS, testes de gamepad ou imagens do display. A suíte host simula a escrita nativa do Nofrendo e **não prova estabilidade dos saves físicos**.

## Riscos impeditivos de release estável

1. **P0 BLE/SD:** os logs anteriores reproduziram falha `EIO` ao escrever SD com advertising ativo; esta edição não implementa modo de manutenção BLE no save. Hardware precisa confirmar slots e backup sob carga.
2. **P0 SD inicialização:** a falha intermitente de `SD.begin` anterior ao BLE não foi comprovadamente corrigida por espera de 120ms. Exige 10 cold boots e verificação de energia, carga/CS e contatos.
3. **P1 Biblioteca Nofrendo não incluída:** core 1.2 com patches locais de PRG/CHR, WDT e áudio é pré-requisito não testado neste ambiente.
4. **P1 FAT e falha elétrica:** `rename()` e SNSS não garantem transações atomicamente à prova de corte de energia.
5. **P2 Formatos NES:** Nofrendo não garante compatibilidade universal de mappers e NES 2.0; não anunciar universalidade.

## Conclusão

A RC2 está pronta para publicação como **código-fonte em pré-release**, com testes host repetíveis e limitações explícitas. Não publicar como firmware 'stable' ou 'testado no hardware' até realizar a matriz de aceite em `TESTS_AND_LIMITATIONS.md`.
