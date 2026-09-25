# Roadmap e Próximos Passos (usb-stream-tv)

## Concluído Hoje (24/09/2026)
1. **Cinema VOD & YouTube na TV (Nativo com Pause, Seek e Retomada)**:
   - Arquitetura **Cloud Virtual Remote Disk via HTTP Range Requests** (Zero Flash local no aparelho).
   - Gerador de FAT32 dinâmico (`gen_template.py`) com LFN e SFN para qualquer tamanho de arquivo até 4 GB.
   - Buffer deslizante de 16 MB em RAM no `fuse_direct.c` com suporte nativo a ARM32 e ARM64.
   - Soft-reset inteligente de barramento USB sem desconexão de cabo (`switch_vod.sh` e `switch_live.sh`).
   - Transcodificação automática para padrão Samsung Plasma PL51F4000: H.264 720p 30fps + Dolby AC3 Stereo 192k 48kHz com `+faststart`.
   - Painel web atualizado com monitoramento de progresso e alternância em 1 clique.
   - Deployed no servidor VPS (`tv.smre.run.place`) e no Xiaomi Mi A2.

## Planejado para Amanhã
1. **Testes Práticos com o Usuário na TV**:
   - Testar reprodução de filmes longos e clipes no ConnectShare com avanço, pausa e retorno ao vivo.
2. **Buffer de TV Ao Vivo (Lead Time de 40-50s)**:
   - Aumentar buffer inicial e margem de segurança do anel circular para evitar erro de "End of file" na TV quando houver instabilidade no upstream IPTV.
3. **Validação no Tablet Samsung Galaxy Tab 3 Lite (SM-T110)**:
   - Testar quando o tablet for ligado e colocado na base da TV.
