# NES-S3 — Demo Offline (ESP32-S3 N16R8)

**Emulador NES portátil com ILI9341, microSD, gamepad BLE Dabble e áudio I2S MAX98357A.**

> **Pré-lançamento: R13.1 Demo RC2 (23/10/2026).** Continuação conservadora da R13 aprovada em testes anteriores do usuário. A RC2 passou em testes C++ no computador, **não** em compilação Arduino-ESP32 integral ou validação física nesta entrega. **Não é uma release estável certificada.**

## Destaques

- **Totalmente offline:** jogos `.nes` armazenados no microSD, sem Wi-Fi, catálogos HTTPS, servidores ou LittleFS.
- Menu 320 × 240 de **duas colunas**: jogos à esquerda e capa `.cov` à direita, sem mudança estética.
- Busca local, favoritos, recentes, continuar jogo e até **500 ROMs indexadas** na pasta `/NES`.
- Nofrendo com ROM carregada uma vez em PSRAM antes da jogatina (máximo 4 MiB por arquivo); evita leitura do microSD durante a emulação da ROM.
- Save states SNSS slots **0–3** e autosave **9**, com recuperação conservadora por `.bak`/`_TMP`.
- DabbleESP32 via BLE; **START+SELECT por 700 ms** abre menu de pausa; áudio MAX98357A I2S.
- Paleta Anemoia Céu Azul e pipeline RGB565 aprovados **preservados**.

## Hardware e pinos

| Periférico | Sinal | GPIO |
|---|---|---|
| TFT + microSD (mesmo HSPI) | SCLK / MOSI / MISO | 12 / 11 / 13 |
| ILI9341 | CS / DC / RST | 10 / 9 / 4 |
| microSD | CS | 14 |
| MAX98357A | BCLK / LRC / DIN | 15 / 16 / 17 |

No `User_Setup.h` da TFT_eSPI use `USE_HSPI_PORT`, e **desative `TOUCH_CS 14`**. Inicie o cartão SD antes do TFT (`SD-FIRST`, 1 MHz); não crie um segundo barramento SPI.

## Instalação rápida

1. Faça backup integral do microSD, principalmente `/NES`, `/covers` e `/save`.
2. Instale ESP32 Arduino Core 3.3.x e selecione **ESP32S3 Dev Module**, CPU 240 MHz, flash **16 MB**, **OPI PSRAM 8 MB**, partições Huge APP ou a configuração anteriormente testada.
3. Instale TFT_eSPI e DabbleESP32. Utilize `arduino-nofrendo` 1.2 **com os patches locais já aprovados** para integridade da ROM, temporização/WDT e áudio. **A biblioteca modificada não está incluída neste repositório.**
4. Ajuste TFT_eSPI de acordo com [hardware](docs/HARDWARE.md).
5. Extraia o pacote, abra [`firmware/NES_S3/NES_S3.ino`](firmware/NES_S3/NES_S3.ino) na Arduino IDE e compile. Não abra o `.ino` dentro do ZIP.
6. Carregue ROMs em `/NES`, capas NS3C em `/covers`, e mantenha `/save` intacto. Use **Upload**, não OpenOCD/Debug. Monitor Serial **115200**.

Veja [instalação completa](docs/INSTALLATION.md), [controles](docs/CONTROLS.md) e [compatibilidade](docs/ROM_COMPATIBILITY.md).

## Estrutura

```text
NES-S3-DEMO-OFFLINE/
├── firmware/NES_S3/          # 15 fontes do sketch Arduino completo
├── docs/                     # instalação, hardware, arquitetura, testes, correções
├── tests/                    # regressões C++ reais (com stubs somente dos periféricos)
├── tools/                    # conferência de integridade
├── release/                  # notas de pré-lançamento
├── licenses/ e references/   # avisos e referências de componentes
├── CHANGELOG.md
├── CREDITS.md
├── THIRD_PARTY_LICENSES.md
├── LICENSE-NOTICE.md
└── VERSION
```

## O que foi efetivamente testado

O teste de host compila `halo_savestates.cpp` e `halo_library.cpp` reais com `g++ -std=c++17 -Wall -Wextra -Werror`, usando stubs para Arduino/Nofrendo. Treze cenários passaram; logs e código estão em [`tests/`](tests/). O resto dos módulos teve revisão estática e comparação com a R13. **Não houve testes de painel, áudio, BLE, ROMs ou gravação física nesta RC2.** Os testes host **não** equivalem a compilar o sketch na Arduino IDE.

### Limites conhecidos e risco importante

**Escrita no microSD com BLE:** a série V4 comprovou falhas de E/S com advertising BLE ativo, e este ramo offline não implementa a manutenção BLE dos downloads R15.8. Portanto, **não está comprovado que salvar estados e favoritos com o Dabble conectado seja confiável**. Faça backup e teste saves antes de qualquer demonstração pública. Falhas intermitentes de `SD.begin` anteriores ao BLE também exigem inspeção elétrica, contatos, pull-ups e power cycle. Consulte [testes e limitações](docs/TESTS_AND_LIMITATIONS.md).

## Créditos, origem e licenças

O projeto integra adaptações do [arduino-nofrendo](https://github.com/moononournation/arduino-nofrendo), conceitos de [ESP32-NESEMU](https://github.com/espressif/esp32-nesemu), biblioteca TFT_eSPI, DabbleESP32 e paleta derivada de Anemoia. Os direitos e termos variam por componente. O firmware **não distribui ROMs**, nem inclui fontes da biblioteca externa modificada do Nofrendo. Leia [créditos](CREDITS.md) e [avisos de licenças](LICENSE-NOTICE.md) antes da redistribuição.

Projeto independente, não afiliado ou endossado pela Nintendo nem pelos autores dos jogos.
