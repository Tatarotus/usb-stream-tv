#!/usr/bin/env python3
"""
test_reproduce_av_desync.py
Caso de Teste Determinístico para o Bug de Descompasso A/V no SeamlessRestamper.

Objetivo:
1. Reconstruir a sequência exata de pacotes MPEG-TS do incidente da Globo RJ:
   - Canal Studio Universal (Canal A): Video e Áudio com PTS inicial = 1.0s (90.000)
   - Chaveamento para Globo RJ (Canal B):
     - Vídeo chega primeiro: in_pts = 75.314.520 (836.828s)
     - Áudio chega depois:   in_pts = 74.492.010 (827.689s)
     (Skew upstream = 9.139s)
2. Passar pelo código ATUAL E INALTERADO do SeamlessRestamper (importado de server.py).
3. Gerar o arquivo MPEG-TS binário: test_desync_incident.ts.
4. Executar verificação determinística e extrair via ffprobe e leitor binário:
   - video PTS
   - audio PTS
   - A/V delta
   - PCR
   - continuity counters
"""

import os
import sys
import subprocess

TS_PKT_SIZE = 188
TS_SYNC_BYTE = 0x47

def encode_ts_timestamp(pts, flags):
    pts = pts & 0x1FFFFFFFF
    b0 = (flags << 4) | (((pts >> 30) & 0x07) << 1) | 1
    b1 = (pts >> 22) & 0xFF
    b2 = (((pts >> 15) & 0x7F) << 1) | 1
    b3 = (pts >> 7) & 0xFF
    b4 = ((pts & 0x7F) << 1) | 1
    return bytes([b0, b1, b2, b3, b4])

def encode_pcr(pcr):
    pcr = pcr & 0x3FFFFFFFFFFF
    base = pcr // 300
    ext = pcr % 300
    b0 = (base >> 25) & 0xFF
    b1 = (base >> 17) & 0xFF
    b2 = (base >> 9) & 0xFF
    b3 = (base >> 1) & 0xFF
    b4 = ((base & 1) << 7) | 0x7E | ((ext >> 8) & 1)
    b5 = ext & 0xFF
    return bytes([b0, b1, b2, b3, b4, b5])

def make_pat():
    """PAT packet: Program 1 -> PMT PID 4096 (0x1000)."""
    pkt = bytearray([0xFF] * 188)
    pkt[0] = 0x47
    pkt[1] = 0x40  # PUSI = 1, PID = 0
    pkt[2] = 0x00
    pkt[3] = 0x10  # payload only, CC = 0
    pkt[4] = 0x00  # pointer field
    # Table ID 0 (PAT), section syntax 1, len 13
    pat = b"\x00\xb0\x0d\x00\x01\xc1\x00\x00\x00\x01\xf0\x00\x2a\xb1\x04\xb2"
    pkt[5:5+len(pat)] = pat
    return bytes(pkt)

def make_pmt():
    """PMT packet: PCR PID 256, Video stream PID 256 (H.264), Audio stream PID 257 (AC3)."""
    pkt = bytearray([0xFF] * 188)
    pkt[0] = 0x47
    pkt[1] = 0x50  # PUSI = 1, PID = 4096 (0x1000)
    pkt[2] = 0x00
    pkt[3] = 0x10  # payload only, CC = 0
    pkt[4] = 0x00  # pointer field
    # PMT section: Program 1, PCR PID 256 (0x100), Program info len 0
    # Stream 1: type 0x1B (H.264), PID 256 (0x100), ES info len 0
    # Stream 2: type 0x06 (AC3), PID 257 (0x101), ES info len 6 (registration descriptor AC-3)
    pmt = (
        b"\x02\xb0\x1d\x00\x01\xc1\x00\x00\xe1\x00\xf0\x00"
        b"\x1b\xe1\x00\xf0\x00"
        b"\x06\xe1\x01\xf0\x06\x05\x04\x41\x43\x2d\x33"
        b"\x37\x3a\x98\x41"  # CRC32 placeholder
    )
    pkt[5:5+len(pmt)] = pmt
    return bytes(pkt)

def make_video_packet(in_pts, cc, pcr=None):
    """Cria pacote TS de vídeo H.264 com payload PES e PCR opcional."""
    pkt = bytearray([0xFF] * 188)
    pkt[0] = 0x47
    pkt[1] = 0x41  # PUSI = 1, PID = 256 (0x100)
    pkt[2] = 0x00
    afc = 3 if pcr is not None else 1
    pkt[3] = (afc << 4) | (cc & 0x0F)
    off = 4
    if pcr is not None:
        pkt[4] = 7  # adaptation field length
        pkt[5] = 0x10  # PCR flag set
        pkt[6:12] = encode_pcr(pcr)
        off = 12
    # PES Header
    pes = bytearray(b"\x00\x00\x01\xe0\x00\x00\x80\x80\x05")
    pes.extend(encode_ts_timestamp(in_pts, 2))
    # NAL unit dummy (SPS/PPS/IDR header)
    pes.extend(b"\x00\x00\x00\x01\x09\xf0\x00\x00\x00\x01\x67\x42\x00\x1f")
    pkt[off:off+len(pes)] = pes
    return bytes(pkt)

def make_audio_packet(in_pts, cc):
    """Cria pacote TS de áudio AC3 com payload PES."""
    pkt = bytearray([0xFF] * 188)
    pkt[0] = 0x47
    pkt[1] = 0x41  # PUSI = 1, PID = 257 (0x101)
    pkt[2] = 0x01
    pkt[3] = 0x10 | (cc & 0x0F)  # payload only
    # PES Header para AC3 (stream ID 0xBD)
    pes = bytearray(b"\x00\x00\x01\xbd\x00\x00\x80\x80\x05")
    pes.extend(encode_ts_timestamp(in_pts, 2))
    # Syncword AC3 (0x0B77)
    pes.extend(b"\x0b\x77\x00\x00\x20\x00")
    pkt[4:4+len(pes)] = pes
    return bytes(pkt)

def run_testcase():
    print("=" * 75)
    print("TESTCASE DETERMINÍSTICO: REPRODUÇÃO DO INCIDENTE A/V SKEW (GLOBO RJ)")
    print("=" * 75)

    # Importa o SeamlessRestamper real do server.py
    sys.path.insert(0, "/home/sam/Code/usb-stream-tv")
    from server import SeamlessRestamper

    restamper = SeamlessRestamper()

    stream_data = bytearray()

    # 1. EMISSÃO INICIAL: Studio Universal (Canal A)
    # 5 frames de vídeo e 5 frames de áudio perfeitamente alinhados em 90.000 ticks (1.000s)
    print("\n[Passo 1] Gerando pacotes do Canal A (Studio Universal - Normal)...")
    for i in range(5):
        v_pts = 90000 + i * 3000
        a_pts = 90000 + i * 2880
        pcr = v_pts * 300
        stream_data.extend(make_pat())
        stream_data.extend(make_pmt())
        stream_data.extend(make_video_packet(v_pts, cc=i, pcr=pcr))
        stream_data.extend(make_audio_packet(a_pts, cc=i))

    out_step1 = restamper.process_chunk(bytes(stream_data))
    print(f"  -> {len(out_step1)} bytes processados pelo Restamper.")

    # 2. CHAVEAMENTO MAKE-BEFORE-BREAK PARA GLOBO RJ (Canal B)
    print("\n[Passo 2] Executando switch para Canal B (Globo RJ)...")
    restamper.start_new_channel()
    print(f"  -> target_base_pts definido para: {restamper.target_base_pts} ({restamper.target_base_pts/90000:.3f}s)")

    # 3. ENTRADA DA GLOBO RJ (Valores exatos do incidente)
    # Vídeo in_pts: 75.314.520 (836.828s)
    # Áudio in_pts: 74.492.010 (827.689s)
    # Skew de entrada do upstream = 822.510 ticks (+9.139s)
    print("\n[Passo 3] Alimentando primeiro chunk da Globo RJ (Vídeo=836.828s, Áudio=827.689s)...")
    ch2_data = bytearray()
    for i in range(10):
        v_pts = 75314520 + i * 3000
        a_pts = 74492010 + i * 2880
        pcr = v_pts * 300
        ch2_data.extend(make_pat())
        ch2_data.extend(make_pmt())
        ch2_data.extend(make_video_packet(v_pts, cc=i, pcr=pcr))
        ch2_data.extend(make_audio_packet(a_pts, cc=i))

    out_step2 = restamper.process_chunk(bytes(ch2_data))
    print(f"  -> {len(out_step2)} bytes processados pelo Restamper.")

    # Salva o fluxo MPEG-TS resultante para análise determinística
    ts_out_path = "/home/sam/Code/usb-stream-tv/test_desync_incident.ts"
    with open(ts_out_path, "wb") as f:
        f.write(out_step1 + out_step2)
    print(f"\n[✓] Arquivo MPEG-TS gravado em: {ts_out_path} ({os.path.getsize(ts_out_path)} bytes)")

    # 4. INSPEÇÃO FORENSE DOS BYTES PRODUZIDOS
    print("\n" + "=" * 75)
    print("AUDITORIA DIRETA DOS PACOTES MPEG-TS PRODUZIDOS PELO RESTAMPER")
    print("=" * 75)

    with open(ts_out_path, "rb") as f:
        ts_bytes = f.read()

    # Analisa cada pacote do Passo 2
    n_pkts = len(ts_bytes) // 188
    found_first_v = False
    found_first_a = False
    v_out_pts = None
    a_out_pts = None
    pcr_val = None
    v_cc = None
    a_cc = None

    # Vamos olhar os pacotes do Passo 2 (a partir do pacote 20)
    for p_idx in range(20, n_pkts):
        pkt = ts_bytes[p_idx * 188 : (p_idx + 1) * 188]
        if pkt[0] != 0x47: continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        pusi = bool(pkt[1] & 0x40)
        afc = (pkt[3] >> 4) & 3
        cc = pkt[3] & 0x0F

        # PCR check
        if pid == 256 and afc in (2, 3) and pkt[4] >= 7 and (pkt[5] & 0x10):
            pcr_b = pkt[6:12]
            base = (pcr_b[0] << 25) | (pcr_b[1] << 17) | (pcr_b[2] << 9) | (pcr_b[3] << 1) | (pcr_b[4] >> 7)
            ext = ((pcr_b[4] & 1) << 8) | pcr_b[5]
            pcr_val = base * 300 + ext

        # PES check
        off = 4
        if afc in (2, 3):
            off += 1 + pkt[4]
        if pusi and off + 14 <= 188 and pkt[off:off+3] == b"\x00\x00\x01":
            sid = pkt[off+3]
            flags2 = pkt[off+7]
            if flags2 & 0x80:
                b = pkt[off+9:off+14]
                pts = (((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1))
                if pid == 256 and not found_first_v:
                    found_first_v = True
                    v_out_pts = pts
                    v_cc = cc
                    raw_v_bytes = b.hex()
                elif pid == 257 and not found_first_a:
                    found_first_a = True
                    a_out_pts = pts
                    a_cc = cc
                    raw_a_bytes = b.hex()

    print(f"PCR do Vídeo (PID 256):    base={pcr_val//300} ticks ({pcr_val//300 / 90000:.3f}s)")
    print(f"Vídeo CC (PID 256):        {v_cc}")
    print(f"Vídeo PTS (PID 256):       {v_out_pts} ticks ({v_out_pts / 90000:.3f}s) | 5 bytes PES: {raw_v_bytes}")
    print(f"Áudio CC (PID 257):        {a_cc}")
    print(f"Áudio PTS (PID 257):       {a_out_pts} ticks ({a_out_pts / 90000:.3f}s) | 5 bytes PES: {raw_a_bytes}")
    
    delta_ticks = v_out_pts - a_out_pts
    delta_secs = delta_ticks / 90000.0
    print("-" * 75)
    print(f"A/V Delta (Vídeo - Áudio): {delta_ticks:+} ticks ({delta_secs:+.3f} segundos / {delta_secs/3600:+.2f} horas)")
    print("-" * 75)

    # 5. EXECUÇÃO VIA FFPROBE (Validação de Ferramenta Externa)
    print("\n[Passo 5] Executando validação externa via ffprobe no arquivo gerado:")
    cmd = [
        "ffprobe", "-v", "error", "-show_entries",
        "packet=pts,pts_time,stream_index,dts,dts_time",
        "-of", "csv=p=0", ts_out_path
    ]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
        lines = [l for l in proc.stdout.strip().split("\n") if l]
        print(f"Total de pacotes decodificados pelo ffprobe: {len(lines)}")
        print("\nPrimeiros pacotes pós-switch analisados pelo demuxer do FFmpeg:")
        for line in lines[20:26]:
            parts = line.split(",")
            # pts,dts,pts_time,dts_time,stream_index
            stream_name = "VÍDEO (0)" if parts[-1] == "0" else "ÁUDIO (1)"
            print(f"  Stream {stream_name}: PTS={parts[0]} ({float(parts[2]):.3f}s)")
    except Exception as e:
        print(f"Erro ao executar ffprobe: {e}")

    print("\n" + "=" * 75)
    if a_out_pts > 8000000000:
        print("💥 CONFIRMAÇÃO DO TESTCASE: O BUG DE 26.5 HORAS FOI REPRODUZIDO COM SUCESSO!")
        print(f"   Áudio gravado no MPEG-TS: {a_out_pts/90000:.3f} segundos ({a_out_pts/90000/3600:.2f} horas)")
        print(f"   Vídeo gravado no MPEG-TS: {v_out_pts/90000:.3f} segundos")
        print(f"   Delta A/V: {delta_secs/3600:.2f} horas")
        print("   Status: REPRODUÇÃO DETERMINÍSTICA COMPLETA E SALVA.")
    else:
        print("[-] Falha ao reproduzir bug.")
    print("=" * 75)

    # Asserção explícita de regressão: A/V skew não pode exceder ±500ms
    MAX_ALLOWED_SKEW_SECS = 0.500
    if abs(delta_secs) >= MAX_ALLOWED_SKEW_SECS:
        print(f"\n❌ FALHA DE ASSERÇÃO OBJETIVA: A/V Skew ({delta_secs:+.3f}s / {delta_secs/3600:+.2f}h) excede o limite de ±{MAX_ALLOWED_SKEW_SECS}s!")
        print("   [ASSERTION_FAILURE] Comportamento defeituoso registrado como evidência comprovada.")
        sys.exit(1)
    else:
        print(f"\n✅ SUCESSO DE ASSERÇÃO: A/V Skew ({delta_secs:+.3f}s) dentro da tolerância de ±{MAX_ALLOWED_SKEW_SECS}s.")
        sys.exit(0)

if __name__ == "__main__":
    run_testcase()

