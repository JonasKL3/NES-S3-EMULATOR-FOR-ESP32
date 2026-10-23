# Diagnóstico de falhas

- `[SD][FATAL]` antes do TFT: verificar alimentação 3,3 V, fiação CS14, MISO13, MOSI11, SCLK12, contatos e cartão. Fazer power-cycle completo; não confiar só no botão RESET.
- `/NES` ausente: criar pasta no microSD FAT, conferir letras maiúsculas e sistema de arquivos.
- Capa ausente: conferir `/covers/Nome exato.cov`, tamanho 28.300 bytes e cabeçalho `NS3C`.
- `NOFRENDO` ou áudio sem som: conferir versão externa modificada da biblioteca e hooks do Nofrendo (ROM integrity/WDT/APU). Não atualizar a biblioteca às cegas.
- `[R13.1][SAVE][ABORT]`: o firmware detectou save inválido ou erro FAT e **preservou os dados**. Fazer backup no computador e examinar `.bak`, `_TMP`, `.badN` antes de retirar arquivos.
- `[R13.1][LIB][ABORT]`: índice principal inválido; cópia `HALO_LIB.BAK` mantida. Faça backup e restaure manualmente um `HALO_LIB.DAT` válido, fora da placa, antes de novas alterações de favoritos/volume.
- Reset inesperado após BLE: inspecionar instalação do Dabble e o histórico de falhas de heap. Não presumir bug resolvido pela versão do sketch.
- Escrita SD `errno=5` / `errno=19`: foi reproduzida na investigação BLE+SD. Não entrar em laço de retries nem formatar cartão; anotar logs e isolar eletricamente.

Monitor Serial **115200**. Anexe logs completos dos erros para reprodutibilidade.
