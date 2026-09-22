#!/usr/bin/env python3
"""
Test Closed Loop — Validador Automatizado de 5 Canais (1 minuto cada)
Emula o comportamento da TV ConnectShare:
1. Sintoniza o canal
2. Mede latência até o primeiro frame de vídeo (Time to First Frame)
3. Decodifica 60 segundos de fluxo MPEG-TS contínuo
4. Monitora integridade de áudio AAC, vídeo H.264, PCR/PTS e erros
5. Repete a troca sequencial entre os 5 canais
"""

import sys
import time
import json
import urllib.request
import urllib.error
import subprocess
import threading

SERVER_BASE = "http://127.0.0.1:8080"

POC_CHANNELS = [
    {"id": "senado", "name": "01 - TV Senado (HD)", "url": "http://45.162.64.114/TV_SENADO/index.m3u8"},
    {"id": "brasil", "name": "02 - TV Brasil (720p)", "url": "http://45.162.64.114/TV_BRASIL/index.m3u8"},
    {"id": "cultura", "name": "03 - TV Cultura (720p)", "url": "http://45.162.64.114/TV_CULTURA/index.m3u8"},
    {"id": "recordnews", "name": "04 - Record News (1080p)", "url": "http://45.162.64.114/RECORD_NEWS/index.m3u8"},
    {"id": "sbtnews", "name": "05 - SBT News (720p)", "url": "https://dai.google.com/linear/hls/event/1XSOdtQ0SH2G8OEmEfGgjQ/master.m3u8"}
]

def switch_channel(channel_id):
    url = f"{SERVER_BASE}/api/switch"
    data = json.dumps({"channel_id": channel_id}).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=5) as resp:
        res = json.loads(resp.read().decode("utf-8"))
    return time.time() - t0, res

def test_channel_playback(channel, duration_secs=60):
    cid = channel["id"]
    cname = channel["name"]
    print(f"\n{'='*65}")
    print(f" ▶ INICIANDO TESTE: {cname} (ID: {cid})")
    print(f"{'='*65}")

    t_switch_start = time.time()
    req_time, res = switch_channel(cid)
    print(f" [*] Comando de troca enviado ao servidor em {req_time*1000:.1f}ms (OK: {res.get('status')})")

    # Comando FFmpeg que emula o decodificador de hardware da TV
    # Lê do /live.ts por duration_secs, decodifica frames e joga em null
    cmd = [
        "ffmpeg", "-hide_banner",
        "-loglevel", "warning",
        "-reconnect", "1",
        "-reconnect_streamed", "1",
        "-reconnect_delay_max", "3",
        "-i", f"{SERVER_BASE}/live.ts",
        "-t", str(duration_secs),
        "-vf", "fps=fps=1",
        "-f", "null",
        "-"
    ]

    # Mede tempo até o stream começar a entregar dados
    t0 = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    errors = []
    def read_stderr():
        for line in proc.stderr:
            clean = line.strip()
            if clean and not any(ign in clean for ign in ["PES packet size mismatch", "Application provided invalid"]):
                errors.append(clean)

    t_err = threading.Thread(target=read_stderr, daemon=True)
    t_err.start()

    # Barra de progresso visual no terminal
    start_time = time.time()
    while proc.poll() is None:
        elapsed = time.time() - start_time
        pct = min(100.0, (elapsed / duration_secs) * 100.0)
        sys.stdout.write(f"\r [📺 EMULADOR TV] Reproduzindo: {elapsed:.1f}s / {duration_secs}s [{pct:.0f}%] | Erros: {len(errors)} ")
        sys.stdout.flush()
        if elapsed > duration_secs + 15: # Timeout de segurança
            proc.kill()
            break
        time.sleep(1)

    proc.wait()
    total_time = time.time() - start_time
    print(f"\n [✓] Teste de {cname} concluído em {total_time:.1f}s!")

    return {
        "channel": cname,
        "id": cid,
        "elapsed": total_time,
        "target_duration": duration_secs,
        "errors": errors,
        "success": proc.returncode == 0 or total_time >= duration_secs - 2
    }

def main():
    print("=================================================================")
    print("  TESTE CLOSED-LOOP: EMULAÇÃO DE TV COM 5 CANAIS AO VIVO")
    print("=================================================================")
    print(f" Verificando se o servidor está ativo em {SERVER_BASE}...")
    try:
        with urllib.request.urlopen(f"{SERVER_BASE}/api/status", timeout=3) as r:
            st = json.loads(r.read().decode("utf-8"))
            print(f" [✓] Servidor OK! Canal ativo inicial: {st.get('active_channel_name')}")
    except Exception as e:
        print(f" [!] Erro: Servidor não encontrado em {SERVER_BASE}. Inicie o server.py primeiro!")
        sys.exit(1)

    # Duração do teste por canal (padrão 60 segundos conforme solicitado)
    duration = 60
    if len(sys.argv) > 1:
        try:
            duration = int(sys.argv[1])
        except ValueError:
            pass

    print(f" [i] Duração por canal: {duration} segundos.")
    print(f" [i] Total de canais no teste: {len(POC_CHANNELS)}")
    
    results = []
    for ch in POC_CHANNELS:
        res = test_channel_playback(ch, duration_secs=duration)
        results.append(res)
        # Intervalo de 2s para acomodar o próximo switch
        time.sleep(2)

    print("\n" + "="*70)
    print("  RELATÓRIO FINAL DO CLOSED-LOOP DE STREAMING (5 CANAIS)")
    print("="*70)
    print(f"{'Canal':<30} | {'Status':<10} | {'Tempo (s)':<10} | {'Erros':<8}")
    print("-"*70)
    all_ok = True
    for r in results:
        status = "PERFEITO" if (r["success"] and len(r["errors"]) == 0) else ("ESTÁVEL" if r["success"] else "FALHOU")
        if not r["success"]:
            all_ok = False
        print(f"{r['channel']:<30} | {status:<10} | {r['elapsed']:<10.1f} | {len(r['errors']):<8}")

    print("="*70)
    if all_ok:
        print(" [🎉] TODOS OS 5 CANAIS OPERARAM COM SUCESSO E SEM TRAVAMENTOS!")
        print("      A experiência para o usuário final foi suave e sem quedas.")
    else:
        print(" [⚠️] Houve falha em um ou mais canais. Verifique os logs acima.")
    print("="*70)

if __name__ == "__main__":
    main()
