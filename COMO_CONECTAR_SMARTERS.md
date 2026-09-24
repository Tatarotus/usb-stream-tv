# Guia de Configuração: IPTV Smarters Pro (Versão Oficial / Limpa)

Este guia orienta como configurar qualquer celular ou tablet Android **sem precisar de root** para funcionar como controle remoto da TV (reproduzindo **Canais Ao Vivo**, **Filmes** e **Séries** na sua TV Samsung via USB).

---

## 1. Onde Baixar o Aplicativo Oficial

Instale a versão oficial (não modificada) do IPTV Smarters Pro em qualquer celular Android:

* **Opção 1 (Play Store):** Procure por **"IPTV Smarters Pro"** ou acerte pelo pacote oficial da *WHMCS SMARTERS*.
* **Opção 2 (APKMirror / Uptodown):** [IPTV Smarters Pro no APKMirror](https://www.apkmirror.com/apk/whmcs-smarters/iptv-smarters-pro/)

> [!NOTE]
> Diferente de versões personalizadas por provedores ("branded"), a versão oficial permite digitar livremente a URL do servidor.

---

## 2. Passo a Passo de Conexão (Login Xtream Codes)

1. Abra o aplicativo **IPTV Smarters Pro**.
2. Aceite os termos de uso.
3. Na tela de tipo de lista, selecione:
   👉 **"CONECTAR COM A API XTREAM CODES"** *(Login with Xtream Codes API)*.
4. Preencha os **4 campos** com os dados abaixo:

| Campo | O que preencher |
| :--- | :--- |
| **Qualquer Nome (Any Name)** | `Controle TV` *(ou o nome que preferir)* |
| **Nome de Usuário (Username)** | `0939303360` |
| **Senha (Password)** | `3811610453` |
| **URL do Servidor / Portal** | **`https://tv.smre.run.place`** |

> [!IMPORTANT]
> - Não esqueça do **`https://`** no início da URL.
> - Se o app solicitar a porta separada da URL, coloque a porta **`443`**.

5. Toque no botão **"ADICIONAR USUÁRIO"** *(Add User)*.

---

## 3. Como Usar para Trocar o Conteúdo na TV

Assim que o login for concluído, o aplicativo exibirá a tela principal com três seções:

* 📺 **Canais Ao Vivo (Live TV):** Acesse as categorias de canais. Ao tocar em qualquer canal, o servidor na VPS sintoniza a transmissão e a TV muda de canal automaticamente.
* 🎬 **Filmes (VOD):** Catálogo de lançamentos, clássicos e novidades com capas e sinopses. Ao clicar em "Assistir", o filme começa a ser transmitido para a TV via streaming USB em 1080p.
* 🍿 **Séries:** Organizadas por temporadas e episódios. Ao clicar no episódio desejado, a TV carrega o episódio selecionado.

---

## 4. Como a Mágica Acontece (Arquitetura)

```
[Celular Novo / Smarters] 
        │ (Wi-Fi ou 4G - Sem Root)
        ▼ Seleciona Canal/Filme/Série via API Xtream
[VPS Oracle Cloud: tv.smre.run.place]
        │ Resolve CDN via Proxy Residencial (Xiaomi Mi A2)
        │ Transcodifica em tempo real (1080p @ 30fps H.264 + Áudio Dolby AC3)
        ▼
[Xiaomi Mi A2 ou Tablet conectado à TV via USB]
        │ FUSE Direct / Ring Buffer circular em memória
        ▼ Emulação de Pen Drive Mass Storage (CANAL AO VIVO.ts)
[TV Samsung Plasma PL51F4000] ➔ Reprodução contínua na TV!
```

---

## 5. Resolução de Problemas

* **Erro de conexão ao salvar:** Verifique se digitou `https://` (com **S**) e não `http://`.
* **Canais funcionam, mas filmes não iniciam:** Verifique se o aparelho residencial (Xiaomi Mi A2 ou Tablet) está conectado ao Wi-Fi de casa, pois ele atua como o túnel residencial para liberar os filmes bloqueados por CDN/Cloudflare.
* **A TV travou no reprodutor:** Pressione `STOP` no controle da TV e abra novamente o arquivo `CANAL AO VIVO.ts`.
