# Instalação detalhada — Arduino IDE

1. Salve um backup completo do cartão SD e também um sketch que já funcionava na sua placa. A pasta `/save` contém dados de progresso e metadados.
2. Arduino IDE → ESP32S3 Dev Module; CPU 240 MHz; Flash Size 16MB; OPI PSRAM habilitada; configuração de partições Huge APP (~3 MB) ou a anteriormente aprovada.
3. Instale Arduino-ESP32 Core 3.3.x, TFT_eSPI, DabbleESP32 e a biblioteca externa `arduino-nofrendo` 1.2 **com patches locais de ROM integrity/WDT/APU**. A biblioteca Nofrendo modificada não está neste ZIP. Não atualize inadvertidamente.
4. Configure o `User_Setup.h` **ativo** da TFT_eSPI (não basta criar cópia solta): `USE_HSPI_PORT`, SCLK12/MISO13/MOSI11, CS10/DC9/RST4. Desabilite `TOUCH_CS` em GPIO14.
5. Formate cartão FAT apenas **antes** de carregar os dados e se necessário; **não formate cartão existente**. Garanta `/NES`, `/covers`, `/save`. Copie os arquivos do seu backup.
6. Abra `firmware/NES_S3/NES_S3.ino` após extrair o ZIP. Compile primeiro. Se compilar, use **Upload**, não Debug/OpenOCD. Serial Monitor 115200 baud.
7. Com o aparelho desligado, insira SD, energize completamente, confira `[SD][MOUNT]`, `[SCAN][DONE]` e `[READY]`. O arquivo da capa é opcional.
8. Teste vídeo, controle e áudio. Somente depois ensaie saves com backup, com o gamepad conectado.

### Estrutura do microSD

```text
microSD/
├── NES/              # jogos .nes
├── covers/           # mesmo nome base + .cov (NS3C 104x136)
└── save/             # estados .ss0..3 e .ss9 + HALO_LIB.DAT; não apagar
```

O formato NS3C contém **12 bytes de cabeçalho + 104×136×2 bytes RGB565 LE = 28.300 bytes**. Não basta renomear PNG/JPG para `.cov`.
