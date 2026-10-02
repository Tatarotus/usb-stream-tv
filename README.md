# USB-Stream-TV

> **Streaming de TV ao vivo (IPTV/HLS) e VOD diretamente para TVs legadas (não-smart) via emulação de pendrive USB Mass Storage (UMS) em hardware Android com root.**

---

## 📺 Visão Geral

O **USB-Stream-TV** soluciona o desafio de levar IPTV e conteúdo sob demanda (VOD) com qualidade digital Full HD (1080p) e áudio Dolby Digital para televisores antigos que possuem apenas porta USB com reprodutor de mídia embutido (ex.: **Samsung ConnectShare / MStar SoC de 2013**), sem necessidade de HDMI, Chromecast, TV Box ou Android TV.

O sistema transforma um dispositivo Android antigo com suporte a OTG/USB Gadget (como o **Samsung Galaxy Tab 3 Lite SM-T110**) em um **pendrive virtual pseudo-infinito**. Enquanto a TV lê continuamente blocos de setores do pendrive acreditando estar reproduzindo um arquivo estático `.ts`, `.trp` ou `.tp`, o motor em espaço de usuário (**FUSE**) injeta os pacotes da transmissão de rede ao vivo em tempo real.

```
┌─────────────────────────────────┐       HTTP Live TS       ┌────────────────────────────────────┐
│      Oracle Cloud VPS Hub       │ ───────────────────────> │  Android Tablet (SM-T110 / Mi A2)  │
│  (FFmpeg + Seamless Restamper)  │    (H.264 + AC-3 48kHz)  │  (/system/xbin/stream_fetcher)     │
└─────────────────────────────────┘                          └─────────────────┬──────────────────┘
                                                                               │
                                                                       FIFO / UNIX Pipe
                                                                               ▼
┌─────────────────────────────────┐    SCSI USB Mass Storage ┌────────────────────────────────────┐
│   Samsung Plasma PL51F4000      │ <─────────────────────── │        FUSE NTFS Driver            │
│ (ConnectShare / MStar SoC 2013) │     Emulated USB Gadget  │  (/system/xbin/fuse_ntfs)          │
└─────────────────────────────────┘   (TV AO VIVO.trp / .tp) └────────────────────────────────────┘
```

---

## ✨ Recursos Principais

- **Emulação USB Mass Storage Inteligente**: Apresenta à TV uma partição NTFS válida de 8.0 GiB contendo o arquivo de transmissão ao vivo (`TV AO VIVO.trp` ou `TV AO VIVO 2.tp`).
- **Mapeamento Circular em RAM com FUSE**: Ring buffer de 128 MiB em RAM que mapeia offsets infinitos de reprodução para a janela ao vivo atual, mantendo um colchão de segurança de 20–25 segundos.
- **Anti-Cache de Hardware Automatizado**:
  - Alternância de nomes no sistema de arquivos NTFS MFT (`TV AO VIVO.trp` ↔ `TV AO VIVO 2.tp`).
  - Randomização do *Volume Serial Number* do setor de boot NTFS a cada inicialização.
  - Alternância da *Inquiry String* SCSI USB (`LIVETV1` ↔ `LIVETV2`).
- **Pipeline de Transcodificação Calibrado para MStar**:
  - H.264 Constrained Baseline / Main Profile Level 4.1 a 1080p@30fps.
  - **Zero B-frames** (`bframes=0`), eliminando qualquer risco de underflow do buffer VBV/CPB.
  - Repetição de SPS/PPS antes de **cada** frame IDR (`repeat-headers=1`).
  - Intervalo de PCR cravado em 20 ms (`-pcr_period 20`), garantindo travamento estável do PLL de 27 MHz da TV.
  - Áudio normalizado em Dolby Digital AC-3 estéreo a 48 kHz (padrão nativo do chip MStar).
- **Controle Remoto Web / PWA**: Interface web responsiva instalável em qualquer smartphone, com troca de canais instantânea via *Make-Before-Break* (sem tela preta).
- **Modos de Operação**:
  - **TV Ao Vivo (Live)**: Transmissão linear contínua.
  - **Cinema (VOD)**: Catálogo sob demanda integrado a YouTube / filmes MP4 locais.
  - **Multi-Canais / Favoritos**: Exposição de diretório virtual com múltiplos canais.

---

## 📂 Estrutura do Repositório

```text
usb-stream-tv-prod/
├── .env.example            # Exemplo de configurações e credenciais (seguro para git)
├── .env                    # Suas credenciais reais e portas (ignorado pelo git)
├── .gitignore              # Proteção estrita contra vazamento de credenciais e caches
├── server.py               # Hub central de streaming (Python / FastAPI / FFmpeg)
├── Dockerfile              # Imagem do servidor para Docker
├── compose.yaml            # Configuração de implantação Docker Compose
├── channels.json           # Grade unificada e higienizada de canais (sem senhas)
├── generate_slate.sh       # Gerador das telas de standby/slate offline
├── dashboard.html          # Controle remoto Web & PWA
├── manifest.json           # Manifesto PWA
├── sw.js                   # Service Worker PWA
├── app-icon-512.png        # Ícone do PWA
├── app-icon.png            # Ícone do PWA
├── src/
│   ├── ntfs/               # Motor do driver FUSE NTFS
│   │   ├── fuse_ntfs.c     # Implementação em C do FUSE NTFS
│   │   ├── fuse_ntfs.h     # Definições de geometria e estruturas NTFS
│   │   └── fuse_ntfs_arm32 # Binário estático compilado para ARM32
│   ├── client/             # Ingestão de rede ultraleve
│   │   ├── stream_fetcher.c # Ingestor TCP socket sem overhead de libc
│   │   └── stream_fetcher_arm32 # Binário estático para ARM32
│   └── tools/              # Utilitários de patch de disco
│       ├── patch_trp.c     # Patcher de MFT Inode 27 para alternar arquivos
│       └── patch_trp_arm32 # Binário estático ARM32
├── scripts/                # Scripts de controle no tablet
│   ├── deploy_tablet.sh    # Deploy automático via ADB
│   ├── switch_tv_mode.sh   # Alternador de modos (Live, Cinema, Favoritos)
│   ├── switch_live.sh      # Atalho para retorno ao Live
│   ├── switch_vod.sh       # Alternador para Modo Cinema
│   ├── reconnect_usb.sh    # Soft-reset do barramento USB
│   └── tv_watchdog.sh      # Daemon de monitoramento, wakelock e CPU governor
├── templates/
│   └── ntfs_template.tar.gz # Template esparso do sistema de arquivos NTFS (8.5 GB)
└── docs/                   # Documentação detalhada
    ├── ARCHITECTURE.md     # Arquitetura do driver FUSE e pipeline
    ├── DEPLOYMENT_GUIDE.md # Guia passo a passo de instalação
    ├── HARDWARE_SPECS.md   # Especificações do decodificador Samsung MStar 2013
    └── TROUBLESHOOTING.md  # Diagnóstico e solução de problemas
```

---

## 🚀 Início Rápido

### 1. Servidor (Oracle VPS / Linux Server)

```bash
# 1. Copiar variáveis de ambiente e preencher credenciais
cp .env.example .env
nano .env

# 2. Iniciar o container Docker
docker compose up -d

# 3. Verificar logs
docker logs -f usb-stream-tv
```

Acesse o painel de controle pelo navegador: `http://<IP_DA_VPS>:8080` (ou via proxy reverso com SSL).

### 2. Dispositivo Android (Tablet / SBC)

Com o tablet conectado ao computador via ADB:

```bash
# Executar o deploy automatizado para o tablet
./scripts/deploy_tablet.sh 127.0.0.1:25555
```

O script cuidará de:
1. Enviar o template NTFS e descompactar em `/data/local/tmp/ntfs_lab/`.
2. Instalar os binários ARM32 estáticos em `/system/xbin/`.
3. Configurar scripts de inicialização, wakelocks e governador de CPU para `performance`.
4. Conectar o LUN USB com identidade `LIVETV1` e expor `TV AO VIVO 2.tp` para a TV.

---

## 📖 Documentação Completa

- [Arquitetura Detalhada](docs/ARCHITECTURE.md)
- [Guia de Implantação Passo a Passo](docs/DEPLOYMENT_GUIDE.md)
- [Especificações de Hardware da TV (MStar 2013)](docs/HARDWARE_SPECS.md)
- [Guia de Diagnóstico e Resolução de Problemas](docs/TROUBLESHOOTING.md)

---

## 📄 Licença

Uso pessoal e experimental dedicado à preservação e extensão de vida útil de televisores legados.
