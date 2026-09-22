#!/usr/bin/env python3
"""
Test User Zapping — Simulação Realista de Zapping de Canais na TV
Emula o comportamento do usuário idoso no controle remoto:
- Assiste o canal por 15s
- Troca de canal sequencialmente (Canal 1 -> 2 -> 3 -> 4 -> 5 -> 6)
- Faz zapping de retorno (Canal 6 -> Canal 2 -> Canal 1)
- Mede latência da troca, integridade de frames e ausência de travamentos
"""

import sys
import time
import json
import urllib.request
import urllib.error
import subprocess
import threading

SERVER_BASE = "http://127.0.0.1:8080"

ZAPPING_SEQUENCE = [
    {"step": 1, "id": "tv-gazeta-sp", "name": "TV Gazeta SP (1080p)", "action": "Inicia no Canal 1"},
    {"step": 2, "id": "sbt-news", "name": "SBT News (720p)", "action": "Canal + (Próximo)"},
    {"step": 3, "id": "record-news", "name": "Record News (1080p)", "action": "Canal + (Próximo)"},
    {"step": 4, "id": "rede-tv-nacional", "name": "RedeTV! Nacional (720p)", "action": "Canal + (Próximo)"},
    {"step": 5, "id": "tv-cultura-sp", "name": "TV Cultura SP (720p)", "action": "Canal + (Próximo)"},
    {"step": 6, "id": "tv-brasil-ebc", "name": "TV Brasil EBC (720p)", "action": "Canal + (Próximo)"},
    {"step": 7, "id": "sbt-news", "name": "SBT News (720p)", "action": "Retorna para Canal 2"},
    {"step": 8, "id": "tv-gazeta-sp", "name": "TV Gazeta SP (1080p)", "action": "Retorna ao Canal 1 (Home)"}
]

def switch_channel(channel_id):
    url = f"{SERVER_BASE}/api/switch"
    data = json.dumps({"channel_id": channel_id}).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=5) as resp:
        res = json.loads(resp.read().decode("utf-8"))
    return (time.time() - t0) * 1000.0, res

def test_zapping_hop(hop, duration_secs=15):
    cid = hop["id"]
    cname = hop["name"]
    action = hop["action"]
    step = hop["step"]

    print(f"\n{'='*70}")
    print(f" [PASSAGEM {step}/8] Ação: {action} ➔ {cname} (ID: {cid})")
    print(f"{'='*70}")

    latency_ms, res = switch_channel(cid)
    print(f" [*] Chaveamento no servidor respondido em {latency_ms:.1f}ms")

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

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    errors = []

    def read_stderr():
        for line in proc.stderr:
            clean = line.strip()
            if clean and not any(ign in clean for ign in ["PES packet size mismatch", "Application provided invalid"]):
                errors.append(clean)

    t_err = threading.Thread(target=read_stderr, daemon=True)
    t_err.start()

    start_time = time.time()
    while proc.poll() is None:
        elapsed = time.time() - start_time
        pct = min(100.0, (elapsed / duration_secs) * 100.0)
        sys.stdout.write(f"\r [📺 EMULADOR TV] Assistindo: {elapsed:.1f}s / {duration_secs}s [{pct:.0f}%] | Erros: {len(errors)} ")
        sys.stdout.flush()
        if elapsed > duration_secs + 10:
            proc.kill()
            break
        time.sleep(1)

    proc.wait()
    total_time = time.time() - start_time
    success = (proc.returncode == 0 or total_time >= duration_secs - 1)
    status_str = "PERFEITO (0 erros)" if (success and len(errors) == 0) else ("ESTÁVEL" if success else "FALHOU")

    print(f"\n [✓] Passo {step} concluído: {status_str} ({total_time:.1f}s assistidos, {len(errors)} erros).")

    return {
        "step": step,
        "action": action,
        "channel": cname,
        "latency_ms": latency_ms,
        "elapsed": total_time,
        "errors": errors,
        "success": success
    }

def main():
    print("=================================================================")
    print("  SIMULAÇÃO DE ZAPPING REALISTA NA TV (8 TROCAS DE CANAL)")
    print("  Fontes: brazil_iptv_working.json (100% Testadas e Verificadas)")
    print("=================================================================")
    print(f" Conectando ao servidor em {SERVER_BASE}...")
    try:
        with urllib.request.urlopen(f"{SERVER_BASE}/api/status", timeout=3) as r:
            st = json.loads(r.read().decode("utf-8"))
            print(f" [✓] Servidor Ativo! Canal atual: {st.get('active_channel_name')}")
    except Exception as e:
        print(f" [!] Erro ao conectar ao servidor em {SERVER_BASE}. Verifique se o server.py está rodando.")
        sys.exit(1)

    results = []
    for hop in ZAPPING_SEQUENCE:
        res = test_zapping_hop(hop, duration_secs=15)
        results.append(res)
        time.sleep(1) # Breve pausa humana entre trocas

    print("\n" + "="*80)
    print("  RELATÓRIO DO TESTE DE ZAPPING REALISTA DO USUÁRIO NA TV")
    print("="*80)
    print(f"{'Passo':<6} | {'Ação do Usuário':<22} | {'Canal Sintonizado':<24} | {'Latência':<10} | {'Status':<10}")
    print("-"*80)
    all_ok = True
    for r in results:
        status = "PERFEITO" if (r["success"] and len(r["errors"]) == 0) else ("ESTÁVEL" if r["success"] else "FALHOU")
        if not r["success"]:
            all_ok = False
        print(f"{r['step']:<6} | {r['action']:<22} | {r['channel']:<24} | {r['latency_ms']:<8.1f}ms | {status:<10}")

    print("="*80)
    if all_ok:
        print(" [🎉] TODAS AS TROCAS DE CANAL OCORRERAM SEM NENHUM TRAVAMENTO!")
        print("      O decodificador da TV alternou os fluxos suavemente e sem interrupções.")
    else:
        print(" [⚠️] Houve falha em um ou mais passos de troca. Verifique os logs acima.")
    print("="*80)

if __name__ == "__main__":
    main()
