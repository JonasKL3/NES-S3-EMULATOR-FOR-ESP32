# Changelog

## R13.1 Demo RC2 — 23/10/2026 (pré-lançamento)
- Proteção de save: não tratar erro de `stat()` como ausência do arquivo; não substituir save inválido sem backup confirmado.
- Proteção de metadata: a gravação de `HALO_LIB.DAT` valida o principal e não sacrifica `HALO_LIB.BAK` quando o principal está corrompido.
- Validação de `HALO_LIB.TMP` agora não modifica o estado na memória.
- Maior tolerância a falhas FAT ao limpar temporários; erros de E/S interrompem as ações destrutivas.
- Sem mudanças nas rotinas de display, cores, áudio, Dabble e Nofrendo.
- Testes host ampliados para 13 cenários, e pacote preparado para GitHub.

## R13.1 Demo RC1 — 22/10/2026
- Margem de 120ms antes da montagem SD e erro EBADF separado de falha física.
- Recuperação de backups e temporários SNSS com proteção `.bad0` a `.bad9`.
- ROM VFS: fechamento do arquivo confirmado; liberação de cache em falhas da tarefa.

## R13 original
- Base local de jogo/menu/capas/áudio/saves de referência aprovada anteriormente pelo autor.
