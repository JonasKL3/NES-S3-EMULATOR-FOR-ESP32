# ROMs e compatibilidade

- Usar arquivos `.nes` diretamente na pasta `/NES` (sem leitura de subpastas na enumeração atual).
- Até **500** entradas são indexadas, em ordem retornada do cartão; o subconjunto lido é ordenado alfabeticamente no firmware.
- O cache em PSRAM aceita arquivo de até **4 MiB** com cabeçalho iNES identificado; verifica tamanhos declarados de PRG/CHR.
- Aceitar `.nes` e cabeçalho válido **não garante** emulação correta de todos os mappers, expansões ou padrões NES 2.0. A compatibilidade efetiva depende da Nofrendo com patches locais, testada por ROM/mapper.
- A ROM é carregada integralmente antes de jogar; não fazer downloads nem escrita de ROM durante gameplay nesta edição.
- Capas `.cov` são opcionais e devem usar o formato NS3C de 104×136 RGB565 little-endian.

Recomendação de teste: duas ROMs locais pequenas com mappers conhecidos compatíveis, uma com capa e outra sem. Não inclua ROMs comerciais neste repositório.
