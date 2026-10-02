# Diretrizes de Governança para Agentes de IA — USB-Stream-TV Production

Este documento estabelece as regras obrigatórias e inegociáveis para qualquer agente de IA que opere, modifique ou mantenha a codebase de produção em `~/Code/usb-stream-tv-prod`.

---

## 1. Regra de Ouro: Higiene Estrita da Codebase

1. **Zero Arquivos Soltos na Raiz**:
   - É **terminantemente proibido** criar scripts temporários de teste, benchmarks pontuais, reproduções ou dumps na raiz do projeto (ex.: `test_*.py`, `reproduce_*.py`, `debug_*.sh`, `dump.bin`).
   - Todos os scripts e testes de diagnóstico pontuais devem residir exclusivamente no diretório temporário do agente (`scratch/`) ou ser executados via comandos one-liner inline no terminal.
2. **Estrutura de Diretórios Protegida**:
   - `server.py`, `Dockerfile`, `compose.yaml`, `channels.json`: Raiz do projeto.
   - `dashboard.html`, `manifest.json`, `sw.js`, ícones PWA: Raiz do projeto (servidos diretamente pelo servidor web).
   - `src/ntfs/`: Código-fonte C e binários do driver FUSE NTFS (`fuse_ntfs.c`, `fuse_ntfs.h`, `fuse_ntfs_arm32`).
   - `src/client/`: Ingestão de rede ultraleve (`stream_fetcher.c`, `stream_fetcher_arm32`).
   - `src/tools/`: Utilitários de sistema de arquivos e MFT (`patch_trp.c`, `patch_trp_arm32`).
   - `scripts/`: Automações operacionais do tablet (`deploy_tablet.sh`, `switch_tv_mode.sh`, `switch_live.sh`, `switch_vod.sh`, `reconnect_usb.sh`, `tv_watchdog.sh`).
   - `templates/`: Templates compactados (`ntfs_template.tar.gz`). **Nunca commitar arquivos `.bin` descompactados de 8.5 GB.**
   - `docs/`: Documentação técnica completa.
3. **Limpeza de Artefatos em Git**:
   - Antes de concluir qualquer tarefa, inspecione `git status`. Remova qualquer arquivo não rastreado que não faça parte formal da arquitetura de produção.
   - O `.gitignore` deve ser sempre respeitado.
4. **Segurança e Sigilo de Credenciais (Zero Secret Leaks)**:
   - É **estritamente proibido** commitar credenciais reais, tokens, senhas ou usuários em arquivos rastreados pelo Git.
   - O arquivo `.env` é reservado exclusivamente para credenciais locais e NUNCA deve ser commitado (protegido no `.gitignore`).
   - Forneça sempre `.env.example` com valores ilustrativos seguros para referência pública no GitHub.
   - Em `channels.json`, URLs de provedores privados devem utilizar estritamente os placeholders `{XTREAM_UPSTREAM}`, `{XTREAM_USER}` e `{XTREAM_PASS}`, que são resolvidos dinamicamente em tempo de execução pelo `server.py`.

---

## 2. Invariantes Técnicos Mandatórios (Decodificador Samsung MStar 2013)

Qualquer alteração no pipeline de vídeo (`server.py`), no driver FUSE (`fuse_ntfs.c`) ou nos scripts de USB deve respeitar estritamente as limitações físicas do decodificador de hardware da TV Samsung Plasma PL51F4000:

### 2.1. Pipeline de Vídeo e Áudio no FFmpeg (`server.py`)
- **Zero B-Frames (`bframes=0`)**: Transmissões ao vivo via emulação USB **devem** ser codificadas com `-tune zerolatency` (`bframes=0`). B-frames causam underflow instantâneo do buffer CPB/VBV no chip MStar quando lidos via SCSI USB com `muxdelay` baixo, congelando a imagem no primeiro frame.
- **Áudio AC-3 Dolby Digital Obrigatório**:
  ```bash
  -c:a ac3 -b:a 384k -ar 48000 -ac 2
  ```
  O firmware 2013 da Samsung não reproduz de forma estável streams de áudio AAC dentro de contêineres MPEG-TS.
- **Intervalo de PCR Estrito**:
  ```bash
  -pcr_period 20
  ```
  Garante que pacotes contendo PCR sejam emitidos a cada 20 ms para travar o PLL de clock STC a 27 MHz da TV.
- **Estrutura de Quadros (GOP)**:
  `-g 30 -keyint_min 30 -sc_threshold 0` com `-x264-params repeat-headers=1`. Um quadro IDR completo com SPS e PPS deve ser entregue exatamente a cada 1.0 segundo.
- **Fatia Única por Quadro**: Nunca repassar transmissões IPTV multi-slice (2 ou mais fatias por frame) sem transcodificar para `slices=1`.

### 2.2. Motor FUSE NTFS (`src/ntfs/fuse_ntfs.c`)
- **Condição de Re-ancoragem Estrita**:
  Reaberturas e saltos de âncora **só podem ocorrer** em `foff == 0`. Leituras sequenciais subsequentes (`foff = 65536`, `131072`, etc.) durante a inicialização do reprodutor ConnectShare nunca devem ser tratadas como reabertura.
- **Pacing Suave Clamped**:
  Quando a margem de segurança for menor que 8 MiB, limitar a entrega de leitura a ~1.0 MB/s, com pausas (`usleep`) sempre clampeadas a $\le 50\text{ ms}$ e mutex `g_mu` destravado durante o sono.
- **Isolamento de Sondagens**:
  Leituras da TV em `foff >= 8MB` que excedam a área escrita em mais de 2 MB devem retornar pacotes MPEG-TS nulos (`PID 0x1FFF: 0x47, 0x1F, 0xFF, 0x10, 0xFF...`) imediatamente, sem alterar o estado do anel nem a âncora.

### 2.3. Subsistema Anti-Cache Triple-Tier
Qualquer reinicialização ou troca para Live TV deve garantir o acionamento dos 3 níveis anti-cache:
1. Executar `patch_trp` para alternar os metadados MFT entre `TV AO VIVO.trp` e `TV AO VIVO 2.tp`.
2. Randomizar o *Volume Serial Number* do setor de boot NTFS (offset `0x48`).
3. Alternar a *Inquiry String* SCSI USB entre `LIVETV1` e `LIVETV2`.

---

## 3. Padrão de Compilação e Deploy no Tablet

1. **Compilação Cruzada ARM32 Estática**:
   Os binários em `src/` devem ser compilados para ARM32 (ARMv7-A) com ligação estática contra glibc/musl:
   ```bash
   arm-linux-gnueabihf-gcc -Wall -Wextra -O2 -static <arquivo.c> -lpthread -o <binario_arm32>
   ```
2. **Deploy Automatizado**:
   Sempre utilize o script mestre de deploy para instalar no tablet:
   ```bash
   ./scripts/deploy_tablet.sh [IP:PORTA_ADB]
   ```
   Nunca deixe processos órfãos (`killall -9 fuse_ntfs stream_fetcher` antes de substituir binários).

---

## 4. Política de Engenharia Multi-Agente

- **Orquestração Primeiro**: Para qualquer diagnóstico, refatoração ou auditoria não-trivial, atue como Líder Técnico. Decomponha o problema em subagentes especializados concorrentes.
- **Posse Disjunta de Arquivos**: Se múltiplos subagentes implementarem alterações em paralelo, eles devem possuir diretórios/arquivos estritamente separados.
- **Verificação Crítica e Antialucinação**: Nunca assuma que uma alteração funcionou sem validar:
  - Compilação estática sem warnings graves.
  - Telemetria em tempo real no tablet (`fuse_ntfs.log` com `near_starve=0`, `nulls=0`).
  - Bitstream inspecionado via `ffprobe` (0 B-frames, áudio AC-3, PCR estável).
