# Auditoria e testes — R13.1 Demo RC2

**Data 23/10/2026. Status PRÉ-RELEASE.**

## Origem

- Fonte baseada no ZIP `NES_S3_R13_1_DEMO_OFFLINE_FIRMWARE_COMPLETO_ARDUINO.zip`.
- Backup R13 original fornecido anteriormente; 15 arquivos no sketch. RC2 altera **3** arquivos relativamente à RC1: `NES_S3.ino` (identificação), `halo_savestates.cpp` (proteção do commit), `halo_library.cpp` (proteção de índice). Demais **12 byte a byte preservados**.
- RC1 já alterara em relação à R13 original `rom_cache_vfs.cpp`, `NES_S3.ino`, `halo_savestates.cpp`. Esses patches foram mantidos, exceto alterações posteriores explícitas.

## Inspeção por módulo

| Componente | Verificação | Resultado e limite |
|---|---|---|
| `NES_S3.ino` | SD-FIRST HSPI, limites dos 500 itens, menu e botão de pausa, PSRAM e tarefas | Revisão estática; não alterados controles nem estética |
| `rom_cache_vfs.cpp` | iNES, leitura com limite 4 MiB, VFS RAM e `fclose` | Igual à RC1; exige teste de ROM no hardware |
| `osd_port.cpp` | OSD Nofrendo, vídeo, paleta, pausa/timer, hooks e save | Idêntico; biblioteca Nofrendo externa impede ensaio total |
| `halo_audio.*` | Fila I2S e MAX98357A | Idêntico; audição/drop físico pendente |
| `nes_s3_covers.*` | Tamanho NS3C, RGB565, cache PSRAM | Idêntico; teste visual pendente |
| `halo_fixed_palette.h`, `nes_color.h` | valores RGB565/índice `$31` | Idênticos; sem retocar cores |
| `halo_savestates.*` | Recovery `.bak`/`_TMP`, evitar sobrescrita de save inválido, `stat`/`remove` defensivos | Código real testado em host; FAT no dispositivo pendente |
| `halo_library.*` | Checksum FNV, backups, roundtrip, não corromper backup válido | Código real testado em host; escrita BLE+SD pendente |
| Dependências externas | `arduino-nofrendo`, Dabble, TFT_eSPI | Não incluídas; sem compilação Arduino válida neste ambiente |
| `LICENSE_GPL3.txt` | Avisos de licença da paleta | Preservado |

## O que FOI testado (reprodutível)

Compilação e execução das implementações reais de `halo_savestates.cpp` + `halo_library.cpp` usando **g++ C++17 com -Wall -Wextra -Werror** e stubs para `Arduino.h`, tipos/entrada de SNSS do Nofrendo. Os stubs não são o código verdadeiro da Nofrendo.

**13 cenários passaram**: (1) primary válido, (2) recuperação BAK, (3) recuperação TMP válido, (4) preservação de primário corrompido sem sobrescrever `.bad0`, (5) fallback TMP, (6) TMP inválido não promovido, (7) estado ausente não inventado, (8) save/load simulado, (9) índice de favoritos/recentes/volume persistido, (10) bloqueio de overwrite de save inválido, (11) gravação com backup já existente, (12) leitura de índice corrompido pelo backup, (13) recusa de sobrescrever o backup com principal inválido.

Log: `tests/TEST_RESULTS_HOST.txt`. Script repetível: `tests/run_host_tests.sh`. GitHub Actions executa só os testes host, **não** compila o firmware do ESP32-S3.

## O que NÃO foi testado nesta entrega

- Compilação integral do sketch na instalação Arduino-ESP32 3.3.x com Nofrendo local modificado.
- Upload, boot real, varredura SD física, cores ILI9341, áudio, latência, Bluetooth, troca de ROM, powercut e compatibilidade de mappers.
- Salvamento SD com Dabble conectado / advertising ativo. **Risco P0 aberto**.
- Dez boots frios para determinar se 120 ms mitigam a montagem SD intermitente. **Risco P0 aberto**.

## Matriz mínima de aceite antes de marcar 'stable'

1. Compile na Arduino IDE usando Nofrendo patched e anexar o log completo.
2. 10 ciclos completos de power-off/on; registrar SD.begin e ROM count.
3. Abrir 5 jogos diferentes (inclusive com e sem capa), testar controles, som e retorno à biblioteca 20 vezes.
4. Verificar slots SNSS 0–3 e 9, reiniciar e reler, com backup e monitor Serial.
5. Testar favoritos/recentes/volume após power cycle; não aceitar `errno=5/19` durante escrita.
6. Medir FPS/drop de áudio, confirmar cores e registrar estabilidade por sessão longa.
7. Verificar que todos os arquivos existentes no SD permanecem íntegros.

Não alegar testes de hardware não executados. Status recomendado: **GitHub Pre-release**.
