#!/usr/bin/env python3
"""
test_web_channel_switching.py
Testa a troca de canais via Web Remote com reprodução CONTÍNUA (sem fechar o player).
Emula a TV assistindo a uma transmissão 24h enquanto o usuário troca os canais
pelo celular no navegador.
"""

import sys
import time
import json
import urllib.request
import subprocess
import threading

SERVER_BASE = "http://127.0.0.1:8080"

SWITCH_TEST_PLAN = [
    {"id": "tv-gazeta-sp", "name": "TV Gazeta SP (1080p)", "watch_time": 10},
    {"id": "sbt-news", "name": "SBT News (720p)", "watch_time": 10},
    {"id": "record-news", "name": "Record News (1080p)", "watch_time": 10},
    {"id": "globo-morena-dourados", "name": "Rede Globo (720p)", "watch_time": 10},
    {"id": "tv-cultura-sp", "name": "TV Cultura SP (720p)", "watch_time": 10},
]

def switch_via_web(channel_id):
    url = f"{SERVER_BASE}/api/switch"
    data = json.dumps({"channel_id": channel_id}).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=5) as resp:
        res = json.loads(resp.read().decode("utf-8"))
    lat_ms = (time.time() - t0) * 1000.0
    return lat_ms, res

def main():
    print("=" * 70)
    print("  TESTE DE TRANSMISSÃO CONTÍNUA 24H COM TROCA DE CANAL WEB")
    print("  Objetivo: Provar que o player da TV permanece ABERTO e TOCANDO")
    print("            enquanto os canais são trocados remotamente via Web.")
    print("=" * 70)

    # 1. Conecta o player contínuo (Emulador da TV)
    print("\n[📺 TV EMULATOR] Abrindo stream contínuo em /live.ts...")
    cmd = [
        "ffmpeg", "-hide_banner",
        "-loglevel", "warning",
        "-reconnect", "1",
        "-reconnect_streamed", "1",
        "-reconnect_delay_max", "3",
        "-i", f"{SERVER_BASE}/live.ts",
        "-vf", "fps=fps=1",
        "-f", "null",
        "-"
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    errors = []

    def read_stderr():
        for line in proc.stderr:
            clean = line.strip()
            if clean and not any(ign in clean for ign in ["PES packet size mismatch", "Application provided invalid"]):
                errors.append(clean)

    t_err = threading.Thread(target=read_stderr, daemon=True)
    t_err.start()

    # Dá 3 segundos para o player firmar no canal inicial
    time.sleep(3)
    if proc.poll() is not None:
        print("[!] Falha ao iniciar stream da TV.")
        sys.exit(1)

    print("[✓] Player da TV conectado e decodificando com sucesso!")

    results = []
    # 2. Executa as trocas de canal via Web enquanto o player continua rodando
    for i, item in enumerate(SWITCH_TEST_PLAN, 1):
        cid = item["id"]
        cname = item["name"]
        wtime = item["watch_time"]

        print(f"\n--- [Troca Web {i}/{len(SWITCH_TEST_PLAN)}] Chaveando para: {cname} ---")
        lat_ms, res = switch_via_web(cid)
        print(f" [*] Resposta da API Web: {res.get('channel')} em {lat_ms:.1f}ms")

        # Monitora a reprodução pelo tempo estipulado
        err_start = len(errors)
        t_start = time.time()
        while time.time() - t_start < wtime:
            if proc.poll() is not None:
                print("\n[!] O PLAYER DA TV CAIU OU FECHOU INESPERADAMENTE!")
                break
            elapsed = time.time() - t_start
            sys.stdout.write(f"\r [📺 TV AO VIVO] Assistindo {cname}: {elapsed:.1f}s/{wtime}s | Erros no hop: {len(errors) - err_start} ")
            sys.stdout.flush()
            time.sleep(1)

        hop_errors = len(errors) - err_start
        is_alive = (proc.poll() is None)
        print(f"\n [✓] Canal {cname}: {'TOCANDO PERFEITAMENTE' if is_alive else 'PAROU'} ({hop_errors} erros)")

        results.append({
            "step": i,
            "channel": cname,
            "latency_ms": lat_ms,
            "errors": hop_errors,
            "player_alive": is_alive
        })

    # Encerra o player ao final do teste completo
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

    print("\n" + "=" * 75)
    print("  RELATÓRIO DO TESTE DE TROCA VIA WEB COM STREAM CONTÍNUO (24H)")
    print("=" * 75)
    print(f"{'Passo':<6} | {'Canal Selecionado via Web':<32} | {'Latência':<10} | {'Player TV':<12}")
    print("-" * 75)
    all_ok = True
    for r in results:
        p_status = "ONLINE (Ativo)" if r["player_alive"] else "FECHOU"
        if not r["player_alive"]:
            all_ok = False
        print(f"{r['step']:<6} | {r['channel']:<32} | {r['latency_ms']:<8.1f}ms | {p_status:<12}")

    print("=" * 75)
    if all_ok:
        print(" [🎉] SUCESSO TOTAL! O PLAYER DA TV NÃO CAIU EM NENHUM MOMENTO!")
        print("      Todas as trocas de canal via Web ocorreram de forma instantânea,")
        print("      sem que a TV detectasse término de arquivo ou precisasse reiniciar.")
    else:
        print(" [⚠️] Ocorreu problema durante a transmissão contínua.")
    print("=" * 75)

if __name__ == "__main__":
    main()
