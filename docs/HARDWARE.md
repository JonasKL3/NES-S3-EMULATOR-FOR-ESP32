# Hardware NES-S3

- **ESP32-S3 N16R8**: flash 16 MB, PSRAM OPI 8 MB, 240 MHz.
- **TFT ILI9341**: 320×240 landscape, rotação 2. GPIO SCK12/MOSI11/MISO13/CS10/DC9/RST4.
- **microSD no slot do display**: compartilha fisicamente SCK12/MOSI11/MISO13 e usa **CS14**. **TOUCH_CS14 desabilitado.**
- **MAX98357A**: I2S BCLK15/LRC16/DIN17, 22020 Hz 16-bit stereo.
- **Controle**: aplicativo Dabble via BLE (nome `ESP32-S3-NES-LAB`).

No TFT_eSPI User_Setup ativo use `USE_HSPI_PORT`; o sketch compila com verificações de pinos para evitar setups incompatíveis. O SD precisa iniciar antes de `tft.init()` e opera a 1 MHz. TFT e SD precisam compartilhar **a mesma SPIClass**, com os CS separados; não ligue os dois CS juntos.

## Alimentação e montagem

- Alimentação 3,3 V correta e estável para o cartão; módulos SD não devem receber 5 V nos pinos de lógica da MCU.
- Fios curtos, bom contato e GND comum para TFT, microSD e ESP32-S3.
- Se falhas intermitentes de `SD.begin` persistirem mesmo antes do BLE, investigue alimentação, carga elétrica, resistores de pull-up adequados, módulo e cartão. Retardar o boot em 120 ms **não** comprova solução.
- Nunca formate o cartão como medida automática de recuperação. Faça backup da pasta `/save`.

Referências: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/sdspi_share.html e https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/sd_pullup_requirements.html.
