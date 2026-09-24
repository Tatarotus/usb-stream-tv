#!/usr/bin/env python3
"""
stream_writer.py — Gravador Circular em Tempo Real para TV via USB
Grava o stream MPEG-TS contínuo do servidor HTTP diretamente no setor
correspondente do pendrive virtual (/data/local/tmp/tv_stream.img).

v2 — IDR-Aligned Wrap + Header Injection at Offset 0
Fixes:
  1. Wraps to offset 0 are aligned to IDR frame boundaries
  2. Every wrap re-injects PAT+PMT+SPS+PPS+IDR at offset 0
  3. Ensures the TV always finds decodable video at file start

Características:
- Escrita circular em anel (Ring Buffer) de 60 MB (~2,7 min de vídeo).
- Nunca transborda para o setor do próximo canal.
- Reconexão automática transparente em caso de instabilidade de rede.
- Chaveamento instantâneo via POST no servidor.
- Tratamento limpo de sinais (SIGTERM/SIGINT) sem deixar processos zumbis.
"""

import os
import sys
import time
import signal
import urllib.request
import urllib.error
import json

# --- TS Packet Parsing Helpers ---

TS_PKT_SIZE = 188
TS_SYNC_BYTE = 0x47
NULL_PID = 0x1FFF

def make_null_packet():
    """Create a single TS null padding packet (PID 0x1FFF)."""
    pkt = bytearray(TS_PKT_SIZE)
    pkt[0] = TS_SYNC_BYTE
    pkt[1] = 0x1F
    pkt[2] = 0xFF
    pkt[3] = 0x10  # no adaptation field, has payload
    return bytes(pkt)

NULL_PKT = make_null_packet()

def get_pid(pkt):
    """Extract PID from a 188-byte TS packet."""
    return ((pkt[1] & 0x1F) << 8) | pkt[2]

def has_pusi(pkt):
    """Check if Payload Unit Start Indicator is set."""
    return bool(pkt[1] & 0x40)

def get_payload(pkt):
    """Extract payload bytes from a TS packet, skipping adaptation field."""
    adapt = (pkt[3] >> 4) & 3
    if adapt == 0 or adapt == 2:
        return b''  # no payload
    if adapt == 1:
        return pkt[4:]
    # adapt == 3: both adaptation field and payload
    af_len = pkt[4]
    return pkt[5 + af_len:]

def find_nal_types(payload):
    """Find NAL unit types in a PES/H.264 payload. Returns set of NAL types."""
    types = set()
    i = 0
    while i < len(payload) - 4:
        if payload[i:i+4] == b'\x00\x00\x00\x01':
            types.add(payload[i+4] & 0x1F)
            i += 5
        elif payload[i:i+3] == b'\x00\x00\x01':
            types.add(payload[i+3] & 0x1F)
            i += 4
        else:
            i += 1
    return types

def chunk_has_idr(data, video_pid=0x100):
    """
    Scan a chunk of TS data for an IDR frame (NAL type 5) preceded by SPS+PPS.
    Returns the byte offset of the PAT packet that precedes the IDR (the ideal
    wrap point), or -1 if no IDR found.
    """
    n_pkts = len(data) // TS_PKT_SIZE
    last_pat_offset = -1
    found_sps = False
    found_pps = False
    
    for i in range(n_pkts):
        offset = i * TS_PKT_SIZE
        pkt = data[offset:offset + TS_PKT_SIZE]
        if len(pkt) < TS_PKT_SIZE or pkt[0] != TS_SYNC_BYTE:
            continue
        
        pid = get_pid(pkt)
        
        if pid == 0x0000:  # PAT
            last_pat_offset = offset
            
        if pid == video_pid and has_pusi(pkt):
            payload = get_payload(pkt)
            nal_types = find_nal_types(payload)
            
            if 7 in nal_types:  # SPS
                found_sps = True
            if 8 in nal_types:  # PPS
                found_pps = True
            if 5 in nal_types and found_sps and found_pps:  # IDR with prior SPS+PPS
                # The ideal split point is the PAT that precedes this IDR
                # If no PAT was found before, use the current packet
                if last_pat_offset >= 0:
                    return last_pat_offset
                return offset
    
    return -1

def find_first_idr_offset(data, video_pid=0x100):
    """
    Find the byte offset of the first PAT or SPS packet that starts a decodable
    sequence (SPS+PPS+IDR) in the data.
    Returns the offset within data, or -1.
    """
    n_pkts = len(data) // TS_PKT_SIZE
    last_pat_offset = -1
    
    for i in range(n_pkts):
        offset = i * TS_PKT_SIZE
        pkt = data[offset:offset + TS_PKT_SIZE]
        if len(pkt) < TS_PKT_SIZE or pkt[0] != TS_SYNC_BYTE:
            continue
        
        pid = get_pid(pkt)
        if pid == 0x0000:
            last_pat_offset = offset
        
        if pid == video_pid and has_pusi(pkt):
            payload = get_payload(pkt)
            nal_types = find_nal_types(payload)
            if 7 in nal_types:  # SPS found — this packet starts a GOP
                if last_pat_offset >= 0:
                    return last_pat_offset
                return offset
    
    return -1


def run_fifo_mode(fifo_path, server_url, channel_id, log_file, pid_file):
    """FUSE-direct feeder: sequential blocking writes of raw TS bytes into
    a named FIFO consumed by fuse_direct. Absolute positions grow forever
    (the ring wraps inside the daemon, positions don't). Deploy order:
    daemon first (it opens the FIFO for reading), then this writer —
    open() below blocks until the daemon is listening (rendezvous)."""
    import signal as _signal

    with open(pid_file, "w") as f:
        f.write(str(os.getpid()))

    running = True

    def _handler(sig, frame):
        nonlocal running
        running = False

    _signal.signal(_signal.SIGTERM, _handler)
    _signal.signal(_signal.SIGINT, _handler)

    def log(msg):
        with open(log_file, "a") as f:
            f.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")

    pos_file = "/data/local/tmp/writer_pos.txt"
    abs_state = "/data/local/tmp/stream_abspos.txt"
    # Resume absolute position across restarts so the daemon's ring
    # mapping never breaks (positions monotonic forever; only content
    # has a small gap at the restart point, like a channel switch).
    try:
        with open(abs_state) as sf:
            abs_pos = int(sf.read().strip().split()[0])
            log(f"Retomando posição absoluta {abs_pos} ({abs_pos/1048576:.1f} MB)")
    except Exception:
        abs_pos = 0
    total = 0
    last_beat = 0.0

    log(f"Modo FIFO: abrindo {fifo_path} (aguarda daemon)...")
    try:
        f_fifo = open(fifo_path, "wb", buffering=0)
    except Exception as e:
        log(f"Falha ao abrir FIFO: {e}")
        return
    log("Modo FIFO: daemon conectado, transmitindo.")

    while running:
        try:
            req = urllib.request.Request(f"{server_url}/live.ts",
                                         headers={"User-Agent": "USBStreamTV/2.0"})
            with urllib.request.urlopen(req, timeout=20) as resp:
                log("Conexão ao vivo estabelecida com sucesso.")
                while running:
                    chunk = resp.read(348 * TS_PKT_SIZE)
                    if not chunk:
                        log("Fim do stream HTTP ou desconexão. Reconectando...")
                        break
                    try:
                        f_fifo.write(chunk)
                    except BrokenPipeError:
                        log("Daemon fechou o FIFO (reiniciou?). Reconectando...")
                        break
                    abs_pos += len(chunk)
                    total += len(chunk)
                    now = time.time()
                    if now - last_beat >= 15:
                        last_beat = now
                        try:
                            with open(pos_file, "w") as pf:
                                pf.write(f"{abs_pos} {total} {channel_id or ''} {time.strftime('%H:%M:%S')}\n")
                            with open(abs_state, "w") as sf:
                                sf.write(f"{abs_pos}\n")
                        except Exception:
                            pass
        except Exception as e:
            if not running:
                break
            log(f"Instabilidade de rede ({e}). Tentando reconectar em 1s...")
            time.sleep(1)

    log(f"Writer FIFO encerrado. Total: {total / 1048576:.1f} MB")
    try:
        f_fifo.close()
    except Exception:
        pass
    try:
        if os.path.exists(pid_file):
            os.remove(pid_file)
    except Exception:
        pass


def main():
    if len(sys.argv) < 3:
        print("Uso: stream_writer.py <start_sector> <server_url> [channel_id]")
        sys.exit(1)

    start_sector = int(sys.argv[1])
    server_url = sys.argv[2].rstrip("/")
    channel_id = sys.argv[3] if len(sys.argv) > 3 else None
    fifo_path = None
    for a in sys.argv[4:]:
        if a.startswith("--fifo="):
            fifo_path = a[len("--fifo="):]

    img_path = "/data/local/tmp/tv_stream.img"
    pid_file = "/data/local/tmp/channel_stream.pid"
    log_file = "/data/local/tmp/channel_stream.log"

    # Salva o próprio PID para gerenciamento externo
    my_pid = os.getpid()
    with open(pid_file, "w") as f:
        f.write(str(my_pid))

    # Buffer circular de 62758912 bytes (59.85 MiB, ~2,7 min a 3 Mbps).
    # 326 * 192512 = LCM(188, 4096): múltiplo exato de pacotes TS e de
    # clusters FAT32 — o fim do arquivo nunca corta um pacote ao meio
    # (62914560 % 188 deixava 172 bytes órfãos no fim).
    # Deve ser múltiplo de 4096 (cluster FAT32) para escrita alinhada.
    BUFFER_SIZE = 62758912
    START_OFFSET = start_sector * 512
    
    # Reserve space at the beginning for the "header zone"
    # This is where PAT+PMT+SPS+PPS+IDR go on every wrap.
    # The header zone is the first 512KB - enough for a full GOP
    HEADER_ZONE = 512 * 1024
    
    # Read chunk size: exactly 348 TS packets (65,424 bytes) — matches TS packet boundary
    READ_CHUNK = 348 * TS_PKT_SIZE  # 65424 bytes

    running = True

    def signal_handler(sig, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)

    def log(msg):
        with open(log_file, "a") as f:
            f.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")

    # 1. Sincroniza o canal ativo do servidor para registro local (nunca força troca upstream)
    try:
        status_req = urllib.request.Request(
            f"{server_url}/api/status",
            headers={"User-Agent": "USBStreamTV/2.0"}
        )
        with urllib.request.urlopen(status_req, timeout=3) as resp:
            status_data = json.loads(resp.read().decode("utf-8"))
            active_ch = status_data.get("active_channel_id")
            if active_ch:
                channel_id = active_ch
                try:
                    with open("/data/local/tmp/current_channel.txt", "w") as cf:
                        cf.write(active_ch)
                except Exception:
                    pass
                log(f"Sincronizado com canal ativo no servidor: {active_ch}")
    except Exception as e:
        log(f"Aviso ao consultar status do servidor: {e}")

    if fifo_path:
        # Modo FUSE-direct: posições absolutas sequenciais num FIFO para o
        # daemon C (fuse_direct). Sem wrap, sem setores, sem header scan
        # (o daemon faz cache dos primeiros 512KB). Backpressure natural:
        # write() bloqueia quando o anel do daemon está cheio.
        run_fifo_mode(fifo_path, server_url, channel_id, log_file, pid_file)
        return

    # 2. Inicia o streaming contínuo gravando no setor circular
    log(f"Iniciando gravação circular v2 (IDR-aligned) no setor {start_sector} (offset {START_OFFSET})...")
    log(f"  Buffer: {BUFFER_SIZE // (1024*1024)} MB, Header zone: {HEADER_ZONE // 1024} KB")

    with open(img_path, "r+b", buffering=0) as f_img:
        # Start writing after the header zone (which will be filled on first wrap)
        curr_offset = START_OFFSET + HEADER_ZONE
        f_img.seek(curr_offset)
        total_written = 0
        last_beat = 0.0
        pos_file = "/data/local/tmp/writer_pos.txt"
        wrap_count = 0
        
        # Track whether we've done the initial header injection
        initial_header_done = False
        
        # Overflow buffer: holds data that couldn't be written before a wrap
        # because we needed to wait for an IDR boundary
        overflow_buf = bytearray()
        
        # Flag: are we waiting for an IDR to wrap?
        waiting_for_idr = False

        while running:
            try:
                stream_url = f"{server_url}/live.ts"
                req = urllib.request.Request(stream_url, headers={"User-Agent": "USBStreamTV/2.0"})
                with urllib.request.urlopen(req, timeout=20) as resp:
                    log("Conexão ao vivo estabelecida com sucesso.")

                    # Every (re)connection starts a new timestamp epoch
                    # (server restart = PTS reset). Re-inject headers at
                    # offset 0 and restart the window there: otherwise the
                    # file keeps the old epoch's tail followed by the new
                    # epoch = backward PTS step that derails players
                    # (mpv mints +2^33 compensations → 26h duration).
                    initial_header_done = False

                    # On first connection, capture headers from the stream
                    # We need to find and store the first SPS+PPS+IDR sequence
                    if not initial_header_done:
                        log("Capturando cabeçalhos iniciais (SPS+PPS+IDR)...")
                        header_buf = bytearray()
                        # Read up to 2MB to find the first IDR
                        while len(header_buf) < 2 * 1024 * 1024 and running:
                            chunk = resp.read(READ_CHUNK)
                            if not chunk:
                                break
                            header_buf.extend(chunk)
                            
                            idr_off = find_first_idr_offset(header_buf)
                            if idr_off >= 0:
                                # Found it! Write everything from the IDR onwards
                                # to the start of the file, and continue writing after.
                                # Pad to a whole number of TS packets so the write
                                # grid stays packet-atomic (a split packet at the
                                # seam hangs picky hardware decoders).
                                header_data = bytes(header_buf[idr_off:])
                                # Truncate to whole TS packets: a split packet at
                                # the seam hangs picky hardware decoders.
                                header_data = header_data[:len(header_data) - (len(header_data) % TS_PKT_SIZE)]
                                
                                # Write the header at offset 0 (start of file data area)
                                f_img.seek(START_OFFSET)
                                f_img.write(header_data)
                                curr_offset = START_OFFSET + len(header_data)
                                total_written += len(header_data)
                                initial_header_done = True
                                log(f"Cabeçalhos injetados no offset 0 ({len(header_data)} bytes, {len(header_data)//TS_PKT_SIZE} pkts)")
                                break
                        
                        if not initial_header_done:
                            log("AVISO: Não encontrou IDR nos primeiros 2MB. Gravando normalmente.")
                            # Write whatever we have
                            if header_buf:
                                f_img.seek(START_OFFSET)
                                f_img.write(bytes(header_buf))
                                curr_offset = START_OFFSET + len(header_buf)
                                total_written += len(header_buf)
                            initial_header_done = True

                        # Fall through to the streaming loop on THIS connection
                        # (a `continue` here would reconnect forever and never
                        # stream, since the flag above resets every connect).

                    # Fresh connection: discard any held wrap-overflow
                    # (bytes from a dead connection must never splice in).
                    overflow_buf = bytearray()
                    waiting_for_idr = False

                    while running:
                        chunk = resp.read(READ_CHUNK)
                        if not chunk:
                            log("Fim do stream HTTP ou desconexão. Reconectando...")
                            overflow_buf = bytearray()
                            waiting_for_idr = False
                            break

                        # HOLD MODE: a wrap is pending and we are waiting for
                        # the next IDR so offset 0 always starts decodable.
                        # (mid-frame seams stall/crash hardware decoders.)
                        if waiting_for_idr:
                            overflow_buf.extend(chunk)
                            idr_off = chunk_has_idr(overflow_buf)
                            if idr_off < 0 and len(overflow_buf) < 400 * 1024:
                                continue  # keep holding (≤ ~1.2s lag max)
                            data = bytes(overflow_buf)
                            overflow_buf = bytearray()
                            waiting_for_idr = False
                            if idr_off < 0:
                                log("Wrap: sem IDR em 400KB, wrap forçado no limite")
                                idr_off = 0
                        else:
                            data = chunk
                            idr_off = -2  # not in hold mode; evaluate below

                        # Check if this write would exceed the buffer boundary
                        end_pos = curr_offset + len(data) - START_OFFSET

                        if end_pos > BUFFER_SIZE and idr_off == -2:
                            # First touch of the boundary: look for IDR.
                            idr_off = chunk_has_idr(data)

                        if end_pos > BUFFER_SIZE:
                            # We're approaching the wrap point.
                            # Strategy: scan the current chunk for an IDR boundary.
                            # Split at the IDR: write pre-IDR to fill the tail,
                            # then wrap and write post-IDR (including headers) at offset 0.

                            if idr_off >= 0:
                                # Found an IDR in this data!
                                # Write everything before the IDR to the tail,
                                # TRUNCATED to what fits (packet-atomic). Bytes
                                # that don't fit are dropped (≤ ~1s gap once
                                # per lap) — never write past the window.
                                space = BUFFER_SIZE - (curr_offset - START_OFFSET)
                                fit = space - (space % TS_PKT_SIZE)
                                pre_idr = data[:idr_off][:max(fit, 0)]
                                post_idr = data[idr_off:]

                                if pre_idr:
                                    f_img.write(pre_idr)
                                    curr_offset += len(pre_idr)
                                    total_written += len(pre_idr)

                                # Fill any remaining space with null TS packets
                                remaining = BUFFER_SIZE - (curr_offset - START_OFFSET)
                                if remaining > 0:
                                    null_fill = NULL_PKT * (remaining // TS_PKT_SIZE)
                                    if null_fill:
                                        f_img.write(null_fill)

                                # WRAP: write post-IDR data at offset 0
                                f_img.seek(START_OFFSET)
                                f_img.write(post_idr)
                                curr_offset = START_OFFSET + len(post_idr)
                                total_written += len(post_idr)
                                wrap_count += 1
                                log(f"Wrap #{wrap_count} alinhado ao IDR no offset 0 ({len(post_idr)} bytes pós-IDR)")
                            else:
                                # No IDR and the boundary is hit: HOLD this data
                                # and keep reading until the next IDR (or 2MB cap).
                                # A forced mid-frame wrap is what stalls the TV.
                                overflow_buf.extend(data)
                                waiting_for_idr = True
                        else:
                            # Normal write — plenty of space
                            f_img.write(data)
                            curr_offset += len(data)
                            total_written += len(data)

                        # Heartbeat de posição (60s): permite ao loop de teste
                        # amostrar exatamente a região recém-gravada
                        now = time.time()
                        if now - last_beat >= 60:
                            last_beat = now
                            try:
                                with open(pos_file, "w") as pf:
                                    pf.write(f"{curr_offset} {total_written} {channel_id or ''} {time.strftime('%H:%M:%S')} w{wrap_count}\n")
                            except Exception:
                                pass

            except Exception as e:
                if not running:
                    break
                log(f"Instabilidade de rede ({e}). Tentando reconectar em 1s...")
                time.sleep(1)

    log(f"Gravador v2 encerrado. Total gravado: {total_written / (1024*1024):.2f} MB, wraps: {wrap_count}")
    try:
        if os.path.exists(pid_file):
            os.remove(pid_file)
    except Exception:
        pass

if __name__ == "__main__":
    main()
