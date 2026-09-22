# USB Stream TV — Infinite Live IPTV on Legacy Non-Smart TVs via Emulated USB Mass Storage

> **Engenharia Reversa & Implementação Completa de Streaming Contínuo para TVs Sem Conexão de Rede**  
> Desenvolvido e validado com sucesso em uma TV de Plasma Samsung PL51F4000 (ConnectShare USB 2.0) usando um Xiaomi Mi A2 (`jasmine_sprout`) com root.

---

## 📺 1. O Desafio e a Visão do Projeto

Televisores antigos (como as lendárias TVs de Plasma Samsung das séries D, E e F, fabricadas entre 2011 e 2014) oferecem excelente qualidade de imagem, mas **não possuem Wi-Fi, Ethernet ou sistema Smart TV**. O único meio de entrada digital além das portas HDMI é a porta **USB ConnectShare**.

O reprodutor **ConnectShare** foi projetado exclusivamente para reproduzir **arquivos estáticos em pendrives FAT32/NTFS** (como filmes em `.mkv` ou `.mp4`). Ele **nunca** foi concebido para transmissões de TV ao vivo.

### A Solução
Transformamos um smartphone ou tablet Android antigo com root em um **Pendrive Virtual Inteligente** que emula um disco USB Mass Storage de 4 GB diretamente na memória RAM (via FUSE). 
* Um servidor central (na VPS ou PC) transcodifica canais IPTV ao vivo para o formato nativo de hardware da TV (H.264 720p/1080p a 30fps com áudio Dolby Digital AC3).
* O celular recebe o fluxo de rede e o serve em tempo real para a TV como se fosse um arquivo `.ts` comum.
* O usuário troca de canal através de um controle remoto web no próprio smartphone, e a TV muda de canal instantaneamente sem fechar o reprodutor de vídeo!

---

## 🚀 2. Os 8 Grandes Breakthroughs Técnicos

Durante o desenvolvimento deste projeto, superamos diversas barreiras de hardware e software que tornaram o projeto viável:

### 1. FUSE Direct em RAM (Zero Desgaste de Memória Flash)
* **O Problema:** Gravar 2.5 Mbps de vídeo continuamente na memória flash interna do celular causaria engasgos de I/O e destruiria o chip de memória eMMC/UFS em poucos meses devido ao limite de ciclos de escrita.
* **A Solução:** Criamos o motor `fuse_direct.c` em C estático. Ele sintetiza um disco virtual FAT32 de 4 GB **100% na memória RAM**. Os setores do vídeo são servidos sob demanda a partir de um anel circular (*ring buffer*) de 32 MB. **Nenhum byte de vídeo toca o flash do aparelho** (apenas logs de texto de alguns KB em `/data/local/tmp/*.log`).

### 2. Superação da Sonda do ConnectShare ("Nenhum Arquivo de Vídeo Encontrado")
* **O Problema:** Antes de exibir qualquer vídeo na lista, o player ConnectShare da Samsung realiza leituras de validação de contêiner lendo nos primeiros 10% do arquivo e próximo ao final (EOF). Se essas leituras retornarem zeros ou pacotes nulos (0x1FFF), a TV declara o arquivo inválido e exibe *"Nenhum arquivo de vídeo encontrado"*.
* **A Solução:** No `fuse_direct.c`, qualquer leitura fora dos limites imediatos da transmissão ao vivo é respondida servindo blocos válidos e cíclicos de pacotes MPEG-TS do próprio anel de RAM. A TV valida os cabeçalhos com sucesso e aceita o vídeo imediatamente.

### 3. Correção do Estouro de Inteiro de 32-bits no FAT32
* **O Problema:** Definir o arquivo virtual com tamanho de 4.29 GB (`0xFFFFD000`) causa *integer overflow* no campo `DIR_FileSize` de implementações de FAT32 de 32 bits com sinal, resultando em um tamanho negativo (`-24.576 bytes`), travando o leitor da TV.
* **A Solução:** Fixamos o tamanho virtual do arquivo `TV AO VIVO.ts` em **1.800.000.000 bytes (~1.8 GB)** com 2.3 GB de espaço livre relatado no setor FSInfo (`gen_template.py`), o que é 100% seguro para qualquer firmware FAT32 legado.

### 4. Modo de Reprodução Infinita (`00:05 / 00:05`)
* **O Funcionamento:** O motor FUSE bloqueia as leituras da TV na fronteira de escrita em tempo real (*1x broadcast rate*). O reprodutor ConnectShare interpreta que a cabeça de leitura chegou ao fim do arquivo virtual, mas como novos dados chegam continuamente a 30fps, a barra de progresso da TV fixa em `00:05 / 00:05` e **continua reproduzindo a transmissão ao vivo indefinidamente sem parar**.

### 5. Re-estampagem Monotônica de Timestamps (`SeamlessRestamper`)
* **O Problema:** Cada canal IPTV possui contadores de continuidade (CC), relógios PCR e marcas de tempo (PTS/DTS) completamente arbitrários. Trocar de canal causa saltos temporais bruscos, travando o decodificador de hardware ou causando desincronização labial de áudio.
* **A Solução:** O `server.py` implementa um re-estampador de pacotes MPEG-TS em tempo real:
  * PIDs fixos e universais: Vídeo em `0x100` (256), Áudio em `0x101` (257) e PMT em `0x1000` (4096).
  * Contador de Continuidade (CC) estritamente sequencial (0 a 15).
  * PCR re-ancorado a 27 MHz e PTS/DTS mapeados em um fluxo temporal contínuo sem saltos para trás.
  * Transcodificação de áudio padronizada para **Dolby Digital (AC3) 48 kHz stereo**, o formato de maior compatibilidade nas TVs Samsung.

### 6. Sincronização Atômica "Make-Before-Break"
* **O Funcionamento:** Ao trocar de canal via interface web (`/api/switch`), o canal anterior **continua transmitindo para a TV** enquanto o novo canal conecta e transcodifica os primeiros quadros em segundo plano. A substituição do fluxo é feita atomicamente no primeiro quadro decodificável (IDR/SPS), garantindo **zero tela preta, zero congelamento e zero interrupção de USB**.

### 7. Supervisão do Gravador contra Morte por Falta de RAM (Watchdog)
* **O Problema:** Com ~84 MB livres (medido no Mi A2), o *Low Memory Killer* do Android mata o `stream_writer.py` sem aviso. Sem ele, a TV congela no último quadro gravado. (Atenção: o watchdog supervisiona o **gravador**, não o gadget USB — ver seção 7.)
* **A Solução:** O `watch_writer.sh` (rodando como root, blindado contra o LMK com `oom_score_adj=-1000`) verifica o gravador a cada 15 segundos e o reinicia em até ~15 s. Ao reiniciar, ele primeiro pergunta ao servidor qual é o canal **ativo** (`/api/status`) para nunca puxar a sintonia de volta após uma troca via web remote.

### 8. Amortecedor de Pacing (`LEADBACK = 12 MB`)
* **O Funcionamento:** A TV lê com uma margem de segurança de **12 MB (~40 segundos)** atrás da escrita ao vivo no anel de RAM de 32 MB. Essa folga atua como um amortecedor hidráulico perfeito, absorvendo oscilações de Wi-Fi, reconexões de rede ou latências de CDN sem que a TV sofra travamentos.

---

## 🏗️ 3. Arquitetura do Sistema

```mermaid
flowchart LR
    subgraph Nuvem / Servidor
        IPTV["Stream IPTV (m3u8 / HLS)"] --> FFMPEG["FFmpeg (720p/1080p 30fps + AC3)"]
        FFMPEG --> HUB["StreamHub + SeamlessRestamper"]
        HUB --> HTTP["Servidor HTTP /live.ts (:8080)"]
    end

    subgraph Celular Android com Root
        HTTP -->|Túnel Cloudflare (HTTPS) ou ADB reverse (127.0.0.1:8080)| WRITER["stream_writer.py --fifo (Cliente HTTP)"]
        WRITER -->|Pipe FIFO /data/local/tmp/live_pipe| FUSE["fuse_direct (Motor C em RAM)"]
        FUSE -->|Buffer 32MB| GADGET["USB ConfigFS (mass_storage.0)"]
        WATCHDOG["watch_writer.sh (vigia o gravador, 15s)"] -.->|Reinicia se morrer| WRITER
    end

    subgraph TV Samsung
        GADGET -->|Cabo USB 2.0| TV["Samsung ConnectShare (PL51F4000)"]
        TV --> DISK["LIVETV -> TV AO VIVO.ts"]
    end
```

---

## 📋 4. Guia Passo a Passo de Replicação

Este guia permite que qualquer outro desenvolvedor ou agente de IA replique este projeto em um novo ambiente ou aparelho (celular/tablet antigo).

### Requisitos Mínimos

1. **Servidor (VPS ou PC Local):**
   * Linux (Ubuntu 22.04 / 24.04 recomendados).
   * FFmpeg compilado com suporte a `libx264` e `ac3`.
   * Python 3.8+.
   * Recursos: 2 vCPUs e 1 GB de RAM livres (para 720p/1080p).
2. **Dispositivo Cliente (Celular ou Tablet):**
   * Aparelho Android com acesso **Root** (Magisk ou SuperSU).
   * Suporte a USB ConfigFS com módulo `mass_storage` no kernel (comum em Android 6 a 10).
   * Aplicativo **Termux** instalado.
   * **RAM livre ≥ 100 MB** (medido: o anel de 32 MB + Python + Android precisam de folga; com ~84 MB livres o LMK já matava o gravador sem o watchdog — confira com `free -m`).
   * Cabo USB de boa qualidade conectado à porta USB da TV.

---

### Passo 1: Configurar o Servidor (VPS ou PC)

1. Clone o repositório no servidor:
   ```bash
   git clone https://github.com/Tatarotus/usb-stream-tv.git
   cd usb-stream-tv
   ```
2. Verifique se o FFmpeg, Python 3 e o compilador cruzado ARM estão instalados:
   ```bash
   sudo apt update && sudo apt install -y ffmpeg python3 python3-pip gcc-aarch64-linux-gnu
   ```
   *(O `gcc-aarch64-linux-gnu` é obrigatório: o `fuse_direct` roda no celular ARM e precisa ser compilado com `-static`.)*
3. Configure seus canais no arquivo `channels.json` (já vem com lista pronta e testada).
4. Inicie o servidor:
   ```bash
   python3 server.py
   ```
   *(Ou execute em segundo plano via systemd com os arquivos em `systemd/` — copie para `~/.config/systemd/user/` e rode `systemctl --user enable --now usb-tv-server usb-tv-tunnel usb-tv-reverse`. Atenção: o túnel Cloudflare gera uma URL nova a cada reinício; anote a URL atual em `tunnel_url.txt` e atualize o `server_url.txt` no celular.)*
5. O painel web estará disponível na porta `8080`:
   * Dashboard e Controle Remoto: `http://SEU_IP_OU_VPS:8080/`
   * Stream contínuo: `http://SEU_IP_OU_VPS:8080/live.ts`

---

### Passo 2: Preparar o Aparelho Android

1. Abra o **Termux** no celular e instale o Python:
   ```bash
   pkg update && pkg install -y python tsu
   ```
2. Verifique se o aparelho suporta USB Mass Storage via ConfigFS:
   ```bash
   su -c 'ls -d /config/usb_gadget/g1/functions/mass_storage.0 || ls -d /sys/kernel/config/usb_gadget/g1/functions/mass_storage.0'
   ```
   *Se o diretório existir ou puder ser criado, o aparelho é 100% compatível!*

---

### Passo 3: Compilar e Enviar os Binários para o Aparelho

Conecte o celular ao PC via cabo USB (com depuração USB ativa) ou via ADB Wi-Fi (`adb connect IP:5555`).

No diretório do projeto no PC, execute o script de deploy automatizado:
```bash
./deploy_to_phone.sh
```

Esse script:
1. Compila o `fuse_direct.c` para a arquitetura do celular (`aarch64` estático).
2. Gera o template FAT32 (`fat_template.bin`).
3. Envia o binário, templates e scripts para `/data/local/tmp/` no celular (< 1 MB total).
4. Cria o comando de atalho `./tv` no diretório inicial do Termux.

> **Atenção:** o deploy só *envia* os arquivos — nada é iniciado. A ordem de boot no celular é rígida: primeiro o daemon (`fuse_direct`, que abre o FIFO para leitura), depois o gravador. O `./tv start` do Passo 4 faz exatamente isso; não inverta.

---

### Passo 4: Conectar na TV e Iniciar a Transmissão

> **CRÍTICO — ordem obrigatória:** o Android **desfaz** o USB Mass Storage e restaura MTP **toda vez** que o cabo USB é reconectado. Por isso configure o gadget **DEPOIS** de plugar na TV, nunca antes. Se a TV mostrar só carregamento ou "nenhum dispositivo", o gadget foi revertido — rode `./tv start` de novo com o cabo já na TV.

1. Conecte o celular na porta **USB** da TV Samsung usando um cabo USB **com fios de dados** (cabos só-de-carga não funcionam; teste o cabo antes com `adb devices` no PC).
2. No celular, abra o aplicativo **Termux** e execute:
   ```bash
   su
   ./tv start http://IP_DA_SUA_VPS:8080
   ```
   *(Com Cloudflare Tunnel, use a URL do túnel em vez do IP — e lembre-se de que ela muda a cada reinício do túnel; veja a seção 7.)*
3. O script irá:
   * Inicializar o motor `fuse_direct` em RAM.
   * Conectar o `stream_writer.py` ao servidor de streaming.
   * Configurar o USB Gadget como pendrive `LIVETV`.
   * Ativar o watchdog de auto-cura.
   * Pré-encher ~15 segundos de buffer antes de liberar o USB (não abra o arquivo na TV antes disso).
4. **Na TV Samsung Plasma:**
   * Pressione a tecla **Source** no controle remoto da TV e selecione **USB (LIVETV)**.
   * Entre na pasta **Vídeos**.
   * Abra o arquivo **`TV AO VIVO.ts`**.
   * Pressione **Play**!
   * Se a TV oferecer "retomar de onde parou", escolha **Não** (a posição salva aponta para dados antigos do buffer circular).
   * *(Opcional)*: Pressione a tecla **Tools** no controle da TV -> *Modo de Repetição* -> *Repetir 1* para garantir reprodução 24h contínua.

---

### Passo 5: Troca Dinâmica de Canais (Zapping Remoto)

Para mudar de canal:
1. Abra o navegador no celular ou no computador e acesse:
   ```text
   http://IP_DA_SUA_VPS:8080
   ```
2. A interface mobile-first exibirá a grade com logos, categorias e programas.
3. Clique em qualquer canal (Globo, Record News, Cultura, Band, ESPN, etc.).
4. A TV comutará o áudio e o vídeo em tempo real sem fechar o arquivo!

---

## 📂 5. Estrutura de Arquivos do Repositório

```text
├── server.py               # Servidor central de streaming, transcodificador e painel web
├── fuse_direct.c           # Motor C FUSE: sintetiza FAT32 em RAM e ring buffer
├── gen_template.py         # Gerador de setores estáticos FAT32 (Boot, FSInfo, Diretório)
├── fat_template.bin        # Template binário compacto do sistema de arquivos FAT32 (5.5 KB)
├── stream_writer.py        # Gravador Python: consome HTTP e alimenta o FIFO em RAM
├── watch_writer.sh         # Watchdog: reinicia o gravador em ≤15s se o LMK matar (somente gravador)
├── usb_tv.sh               # Script mestre de controle no celular (./tv start|stop|status)
├── on_channel_switch.sh    # Sonda servidor (USB→LAN→túnel) e (re)inicia o gravador no canal
├── prefill_head.py         # Utilitário: pré-grava 16MB ao vivo no início do arquivo (modo legado)
├── deploy_to_phone.sh      # Script de compilação cruzada e deploy automático via ADB
├── deploy_fixed.sh         # Deploy da variante legada (imagem estática + writer por setores)
├── systemd/                # Unidades user: usb-tv-{server,tunnel,reverse}.service
├── fuse_direct_SPEC.md     # Especificação de arquitetura do motor FUSE (leitura obrigatória p/ devs)
├── channels.json           # Grade de canais IPTV com metadados e logos
├── sync_iptv.py            # Atualizador e validador automático de streams IPTV
├── keep-adb-reverse.sh     # Manutenção de túnel de desenvolvimento local ADB
└── README.md               # Documentação técnica e guia de replicação
```

---

## 🛠️ 6. Diagnóstico e Resolução de Problemas

| Sintoma | Causa Provável | Solução |
| :--- | :--- | :--- |
| **A TV exibe "Nenhum arquivo"** | Leitura de validação do ConnectShare retornou dados vazios. | Certifique-se de que o `fuse_direct` está rodando e o template `fat_template.bin` está presente em `/data/local/tmp/`. |
| **O celular apenas carrega na TV** | O Android desfez o Mass Storage (acontece em **todo** replug de cabo, e ao desligar a TV). O watchdog **não** monitora o gadget, só o gravador. | Rode `./tv start` no Termux **com o cabo já plugado na TV**. |
| **O vídeo congela após alguns minutos** | (a) Gravador morto pelo LMK; (b) salto de timestamp da fonte; (c) Wi-Fi fraco. | (a) `./tv status` — o watchdog já deve ter reiniciado; confira `channel_stream.log`. (b) Confira `server_events.log` por `PTS_JUMP`/`PTS_REBASE` — o servidor suaviza sozinho. (c) O `LEADBACK` de 12 MB absorve oscilações curtas; garanta Wi-Fi forte. |
| **Áudio mudo na TV** | Codec de áudio incompatível. | O `server.py` converte obrigatoriamente para AC3 (Dolby Digital) a 48 kHz, compatível com 100% das TVs Samsung. |
| **Imagem antiga/congelada ao abrir** | Posição de resume salva pela TV, ou gravador parado. | Recuse "retomar", reabra do zero. Se persistir: `./tv status` no Termux e confira timestamps em `channel_stream.log`. |
| **Duração absurda no player (ex.: 26h)** | Salto de timestamp da fonte ao vivo. | O `SeamlessRestamper` registra em `server_events.log` e suaviza; aguarde uma volta do buffer (~3 min). |
| **Gravador em loop de reconexão** | URL do túnel expirou (muda a cada reinício do cloudflared) ou `adb reverse` caiu. | Atualize `/data/local/tmp/server_url.txt` com a URL atual de `tunnel_url.txt`; no PC, confira `systemctl --user status usb-tv-reverse`. |
| **PC não monta / adb sumiu após mexer no USB** | Todo `echo none > .../UDC` derruba o transporte ADB até re-enumerar. | Aguarde ~10 s e `adb wait-for-device`. Nunca assuma ADB vivo logo após reconfigurar o gadget. |

> **⚠️ Aviso de segurança:** este repositório é público e o `channels.json` contém URLs de providers IPTV (possivelmente com credenciais). Antes de publicar um fork, remova credenciais e use variáveis de ambiente ou arquivo local ignorado pelo git.

---

## 📜 Licença e Créditos
Desenvolvido com engenharia de precisão e testes em hardware real. Sinta-se livre para replicar, modificar e expandir para outras marcas de televisores legados!
