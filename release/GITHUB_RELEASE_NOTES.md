# NES-S3 R13.1 Demo Offline — RC2

**Versão candidata para demonstração presencial, totalmente sem Wi-Fi.**

### O que contém
- Navegador local de até 500 jogos NES, capa NS3C, busca, favoritos e recentes.
- Dabble BLE, áudio I2S MAX98357A, menu de pausa START+SELECT, saves 0–3 e autosave 9.
- Melhorias de recuperação de arquivos e proteção dos backups quando o SD apresenta erros.
- Firmware completo para abrir na Arduino IDE, documentação de hardware e testes host reproduzíveis.

### Instalando
Baixe o ZIP da versão, **extraia** e abra `firmware/NES_S3/NES_S3.ino`. Use ESP32S3 Dev Module N16R8, TFT_eSPI HSPI pinos 12/13/11 e biblioteca Nofrendo 1.2 com patches locais conhecidos; leia `README.md` e `docs/INSTALLATION.md` antes.

### Estado
**PRE-RELEASE / RC2.** 13 testes host passaram. O firmware completo **não foi compilado com o core ESP32 nem validado em placa nesta entrega**. Os saves com BLE conectado e o `SD.begin` ocasionalmente falho seguem como riscos em aberto. Faça backup do cartão SD; não substituir ainda a versão estável do autor.

### Créditos
Nofrendo, TFT_eSPI, DabbleESP32 e Anemoia conforme CREDITS/THIRD_PARTY_LICENSES. Sem ROMs no ZIP.
