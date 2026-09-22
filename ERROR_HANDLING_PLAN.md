# Error Handling & Stability Fix Plan (USB Stream TV)

## 1. Contexto e Resultados do Teste Closed-Loop (5 Canais)

Executamos uma bateria de testes em malha fechada (*closed-loop test*) emulando o decodificador de hardware da TV Samsung ConnectShare em 5 canais abertos nacionais (60 a 75 segundos contínuos cada):

| Canal | Duração Testada | Status | Erros Detectados | Comportamento Observado |
| :--- | :--- | :--- | :--- | :--- |
| **01 - TV Senado (HD)** | 65.0s | ESTÁVEL | 1.136 | Começou com 0 erros nos primeiros 15s; acumulou erros ao final da troca |
| **02 - TV Brasil (720p)** | 64.0s | ESTÁVEL | 3.567 | Reproduziu até o fim, mas com alertas contínuos de DTS discontinuity |
| **03 - TV Cultura (720p)** | 66.0s | ESTÁVEL | 3.604 | Reproduziu até o fim com alertas contínuos de DTS discontinuity |
| **04 - Record News (1080p)** | 62.0s | ESTÁVEL | 3.594 | Reproduziu até o fim com alertas contínuos de DTS discontinuity |
| **05 - SBT News (720p)** | 75.0s | **PERFEITO** | **0** | **100% liso, 0 erros, timestamps perfeitos** |

---

## 2. Diagnóstico das Causas-Raiz (Root Cause Analysis)

### Causa-Raiz 1: Race Condition Assíncrona no `switch_channel()`
* **Mecanismo da Falha**: 
  Quando `switch_channel()` era acionado via API HTTP:
  1. Chamava imediatamente `self.restamper.start_new_channel()`, que resetava `self.first_pts_in_epoch = None`.
  2. Em seguida, chamava `self.proc.terminate()` no processo antigo do FFmpeg.
  3. Porém, `terminate()` é **assíncrono**. O processo antigo ainda tinha dezenas de pacotes residuais no buffer de saída (`pipe:1`).
  4. A thread `_worker` lia esses pacotes residuais do canal antigo (que já estava rodando há 60 segundos com PTS alto, ex: `5.400.000` = ~60s).
  5. O `SeamlessRestamper` capturava esse PTS alto do canal moribundo e definia `self.pts_offset = 90000 - 5400000 = -5.310.000`.
  6. Milissegundos depois, o processo novo do FFmpeg finalmente iniciava com pacotes começando em `PTS = 0`.
  7. Ao aplicar o offset negativo nos novos pacotes (`0 + (-5.310.000)`), ocorria um underflow de inteiros de 33 bits:
     $$(-5.310.000) \pmod{2^{33}} = 8.584.459.465$$
  8. O decodificador da TV recebia pacotes de áudio/vídeo com timestamp de **8,5 bilhões de unidades** (26,5 horas no futuro!), gerando descontinuidade fatal e congelamento.
* **Por que o SBT News deu 0 erros?**
  O SBT News foi o último canal testado. Não houve nenhuma troca de canal após ele, de modo que nenhum processo posterior sofreu contaminação de buffer.

### Causa-Raiz 2: Underflow sem Clamp no Cálculo de Timestamps
* **Mecanismo da Falha**:
  No cálculo de `out_pts`, `out_dts` e `out_pcr`, o operador bitwise `& 0x1FFFFFFFF` converte qualquer valor negativo de Python em um número próximo de $2^{33}-1$. 
  Faltava uma trava defensiva (`clamp`) para impedir que valores negativos virassem números astronômicos.

### Causa-Raiz 3: Typo no Finalizador do Script de Teste
* **Mecanismo da Falha**:
  O arquivo `test_closed_loop.py` continha um literal `EOF` na linha 157 herdado de uma criação via heredoc, gerando um `NameError: name 'EOF' is not defined` ao término da execução do relatório.

---

## 3. Plano de Correção Implementado

### A. Sincronização Estrita do Ciclo de Vida do FFmpeg (`server.py`)
1. **Removido reset prematuro**: O `switch_channel()` **não** mais chama `self.restamper.start_new_channel()` nem esvazia filas enquanto o processo antigo ainda respira. Ele apenas mata o processo antigo (`self.proc.kill()`) e sinaliza `self.switch_event.set()`.
2. **Reset no início da nova época**: O `_worker` aguarda a morte completa do processo anterior. Antes de iniciar o novo comando FFmpeg, com a garantia de que o pipe antigo está vazio, ele:
   - Limpa completamente as filas de todos os assinantes (`q.get_nowait()`);
   - Chama `self.restamper.start_new_channel()`;
   - Inicia o novo subprocesso FFmpeg.
3. **Isolamento garantido**: O primeiro pacote lido pelo `SeamlessRestamper` agora pertence com 100% de certeza ao novo canal.

### B. Proteção Defensiva Contra Underflow (`max(0, ...)`)
No `SeamlessRestamper.process_chunk()`:
```python
# PTS
raw_pts = in_pts + self.pts_offset
out_pts = max(0, raw_pts) & 0x1FFFFFFFF

# DTS
raw_dts = in_dts + self.pts_offset
out_dts = max(0, raw_dts) & 0x1FFFFFFFF

# PCR
raw_pcr = in_pcr + self.pcr_offset
out_pcr = max(0, raw_pcr) & 0x1FFFFFFFFFFFF
```
Isso impede matematicamente que qualquer cálculo residual resulte em timestamps gigantescos de 33 bits.

### C. Correção do Runner `test_closed_loop.py`
Removido o `EOF` literal da última linha, garantindo saída limpa com código de retorno `0`.

---

## 4. Próximos Passos para a Experiência do Usuário Final (Idosos)

1. **Validação Contínua**:
   - Rodar bateria de troca rápida de canal (15s cada) para confirmar que todos os 5 canais operam com status **PERFEITO (0 erros)**.
2. **Migração do Sensor FUSE (v2 - Ring Buffer)**:
   - Para a experiência final na TV, manter a imagem FAT32 com os arquivos `.ts` mapeados e alimentar a leitura via memória RAM (ring buffer), eliminando o delay de disco.
3. **Tempo de Troca Alvo**:
   - Troca de canal entre 1.5s e 2.5s na TV, sem congelamentos e com áudio e vídeo instantaneamente sincronizados.
