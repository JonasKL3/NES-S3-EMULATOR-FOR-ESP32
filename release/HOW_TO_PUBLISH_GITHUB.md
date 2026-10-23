# Como publicar esta pré-release no GitHub

1. Crie um repositório público separado para o NES-S3 offline (exemplo de nome: `NES-S3-Demo-Offline`). **Não utilize `nes-s3-catalogo`**, que tem outra finalidade.
2. Extraia o ZIP `NES_S3_R13_1_DEMO_RC2_GITHUB_PUBLICACAO.zip`. Envie as **pastas e arquivos da raiz** para o repositório, preservando `.github/workflows/host-tests.yml` e o diretório `firmware/NES_S3`.
3. Confira o README e a árvore de arquivos. No GitHub, veja se o workflow `C++ host regression tests (not hardware)` ficou verde. Lembre que isso testa somente os módulos de saves e biblioteca com stubs.
4. Abra `Releases` → `Draft a new release`. Tag sugerida: `v0.2.9.11-r13.1-demo-rc2`. Título: `NES-S3 Demo Offline — RC2 (Pré-release)`.
5. Cole o conteúdo de `release/GITHUB_RELEASE_NOTES.md` na descrição, anexe o ZIP Arduino IDE separadamente e **marque "Set as a pre-release"**.
6. Só altere a classificação para 'estável' após compilar no ambiente real e concluir os testes físicos (principalmente SD/BLE).

Este pacote foi **preparado localmente**. Nenhum push/release foi realizado na conta GitHub. O acesso do assistente ao repositório exibiu permissões somente de leitura.
