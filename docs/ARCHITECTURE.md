# Arquitetura da edição offline

`setup()` inicializa HSPI/SD **antes** do TFT; depois aloca índice na PSRAM, varre somente nomes em `/NES`, carrega preferências, áudio I2S e controle Dabble.

Fluxo ao selecionar jogo:

1. O menu seleciona um caminho da biblioteca local.
2. `HaloRomCache::load()` faz leitura integral SD→PSRAM; verifica iNES e hash FNV payload.
3. O VFS RAM `/romcache/GAME.NES` fornece a ROM ao núcleo Nofrendo, sem ler SD durante busca dos bancos.
4. `osd_port.cpp` implementa vídeo, entrada, pausa e hooks exigidos pela Nofrendo com patches locais.
5. `HaloAudio` recebe áudio por hook da emulação e envia ao MAX98357A por uma fila I2S.
6. Estados SNSS são persistidos em `/save` usando `HaloSave`, com backup e temporários. Biblioteca de favoritos/recentes/volume usa `HALO_LIB.DAT`.
7. Ao sair, a tarefa sinaliza o `loop()`; ele encerra áudio da sessão, libera PSRAM da ROM e reconstrói o menu.

**Núcleo e display travados:** a RC2 não altera `osd_port.cpp`, `halo_audio.cpp`, `nes_s3_covers.cpp`, paleta nem controles. Não reintroduz funções Wi-Fi/HTTPS.

## Atenção concorrência / persistência

O ROM cache é somente leitura após carregado, mas os saves ainda gravam no microSD com Dabble ativo. Observações anteriores indicam possibilidade de `EIO` nessa condição. A recuperação de arquivo lógico não substitui o diagnóstico do barramento SPI e BLE.
