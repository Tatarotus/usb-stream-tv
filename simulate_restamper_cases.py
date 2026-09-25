#!/usr/bin/env python3
"""
simulate_restamper_cases.py
Simulação Matemática e Teste de Bancada dos 7 Casos de Invariante A/V
para o SeamlessRestamper sem alterar server.py.

Casos Avaliados:
- Caso 1: Vídeo e áudio perfeitamente alinhados (delta = 0 ms).
- Caso 2: Áudio 100 ms antes do vídeo (delta = -100 ms, interleaving normal).
- Caso 3: Áudio 500 ms antes do vídeo (delta = -500 ms, limite operacional).
- Caso 4: Áudio 1.0 s antes do vídeo (delta = -1000 ms, fora do limite operacional).
- Caso 5: Áudio 9,14 s antes do vídeo (delta = -9139 ms, o incidente real).
- Caso 6: Áudio 9,14 s depois do vídeo (delta = +9139 ms, skew patológico positivo).
- Caso 7: Troca de canal próxima ao wrap de 33 bits (PTS anterior = 2^33 - 100.000).

Para cada caso, este script calcula e valida:
input A/V delta -> output A/V delta -> video PTS -> audio PTS -> PCR -> monotonicidade -> resultado esperado.
"""

import sys

TICKS_PER_SEC = 90000
WRAP33_MOD = 1 << 33  # 8589934592
OPERATIONAL_MAX_SKEW_TICKS = 45000  # 500 ms threshold operacional

def wrap33(v):
    return v % WRAP33_MOD

def signed_diff_33(a, b):
    """Calcula a diferença modular com sinal (a - b) no espaço de 33 bits [-2^32, 2^32 - 1]."""
    diff = (a - b) % WRAP33_MOD
    if diff >= (1 << 32):
        diff -= WRAP33_MOD
    return diff

class PhaseCoherentRestamperSimulator:
    """
    Simulador da política de invariante A/V para o SeamlessRestamper:
    - Preserva relação A/V se |skew| <= OPERATIONAL_MAX_SKEW_TICKS (Caso A).
    - Se |skew| > OPERATIONAL_MAX_SKEW_TICKS (Caso B), declara descontinuidade de fase upstream
      e inicia nova fase A/V coerente alinhada ao relógio mestre (vídeo).
    - Nunca produz valores negativos em cálculos intermediários.
    """
    def __init__(self, initial_max_pts=90000):
        self.max_pts_seen = initial_max_pts
        self.target_base_pts = wrap33(initial_max_pts + 3000)
        self.first_video_in_pts = None
        self.first_audio_in_pts = None
        self.video_pts_offset = None
        self.audio_pts_offset = None
        self.last_out_video_pts = None
        self.last_out_audio_pts = None
        self.phase_realignment_triggered = False

    def start_new_channel(self, base_pts=None):
        if base_pts is not None:
            self.max_pts_seen = base_pts
        self.target_base_pts = wrap33(self.max_pts_seen + 3000)
        self.first_video_in_pts = None
        self.first_audio_in_pts = None
        self.video_pts_offset = None
        self.audio_pts_offset = None
        self.last_out_video_pts = None
        self.last_out_audio_pts = None
        self.phase_realignment_triggered = False

    def process_video_pts(self, in_pts):
        if self.first_video_in_pts is None:
            self.first_video_in_pts = in_pts
            # video_pts_offset = (target_base_pts - in_pts) mod 2^33
            self.video_pts_offset = wrap33(self.target_base_pts - in_pts)

        out_pts = wrap33(in_pts + self.video_pts_offset)
        self.last_out_video_pts = out_pts
        if signed_diff_33(out_pts, self.max_pts_seen) > 0:
            self.max_pts_seen = out_pts
        return out_pts

    def process_audio_pts(self, in_pts):
        if self.first_audio_in_pts is None:
            self.first_audio_in_pts = in_pts

            # Avalia a relação de fase com o vídeo (se o vídeo já ancorou a época)
            if self.first_video_in_pts is not None:
                skew_in = signed_diff_33(in_pts, self.first_video_in_pts)
                if abs(skew_in) <= OPERATIONAL_MAX_SKEW_TICKS:
                    # Caso A: Skew aceitável (<= 500ms) -> Preserva relação temporal original do upstream
                    self.audio_pts_offset = self.video_pts_offset
                    self.phase_realignment_triggered = False
                else:
                    # Caso B: Skew patológico (> 500ms) -> Descontinuidade de fase do upstream.
                    # Inicia nova fase A/V coerente alinhada ao relógio mestre (target_base_pts)
                    self.audio_pts_offset = wrap33(self.target_base_pts - in_pts)
                    self.phase_realignment_triggered = True
            else:
                # Áudio chegou antes do vídeo: ancora provisoriamente
                self.audio_pts_offset = wrap33(self.target_base_pts - in_pts)

        out_pts = wrap33(in_pts + self.audio_pts_offset)

        # Monotonicidade estrita de áudio
        if self.last_out_audio_pts is not None:
            diff_last = signed_diff_33(out_pts, self.last_out_audio_pts)
            if diff_last <= 0:
                out_pts = wrap33(self.last_out_audio_pts + 2880)

        self.last_out_audio_pts = out_pts
        if signed_diff_33(out_pts, self.max_pts_seen) > 0:
            self.max_pts_seen = out_pts
        return out_pts

def run_simulation():
    print("=" * 105)
    print("SIMULAÇÃO DE ENGENHARIA: MATRIZ DE TESTE DOS 7 CASOS DE INVARIANTE A/V")
    print("=" * 105)

    cases = [
        {
            "id": 1,
            "title": "Caso 1: Vídeo e áudio perfeitamente alinhados",
            "v_in": 1000000,
            "a_in": 1000000,
            "desc": "Alinhamento perfeito na entrada",
            "expected_policy": "Preservar relação A/V (delta = 0ms)"
        },
        {
            "id": 2,
            "title": "Caso 2: Áudio 100 ms antes do vídeo",
            "v_in": 1000000,
            "a_in": 1000000 - int(0.100 * TICKS_PER_SEC),
            "desc": "Interleaving normal de GOP (-100ms)",
            "expected_policy": "Preservar relação A/V (delta = -100ms)"
        },
        {
            "id": 3,
            "title": "Caso 3: Áudio 500 ms antes do vídeo",
            "v_in": 1000000,
            "a_in": 1000000 - int(0.500 * TICKS_PER_SEC),
            "desc": "No limite do threshold operacional (-500ms)",
            "expected_policy": "Preservar relação A/V (delta = -500ms)"
        },
        {
            "id": 4,
            "title": "Caso 4: Áudio 1.0 s antes do vídeo",
            "v_in": 1000000,
            "a_in": 1000000 - int(1.000 * TICKS_PER_SEC),
            "desc": "Acima do threshold operacional (-1.0s)",
            "expected_policy": "Realinhar fase (delta ~ 0ms)"
        },
        {
            "id": 5,
            "title": "Caso 5: Áudio 9,14 s antes do vídeo (Incidente Globo RJ)",
            "v_in": 75314520,
            "a_in": 74492010,
            "desc": "Incidente real (-9.139s)",
            "expected_policy": "Realinhar fase (delta ~ 0ms, sem wrap de 26.5h)"
        },
        {
            "id": 6,
            "title": "Caso 6: Áudio 9,14 s depois do vídeo",
            "v_in": 74492010,
            "a_in": 75314520,
            "desc": "Skew patológico positivo (+9.139s)",
            "expected_policy": "Realinhar fase (delta ~ 0ms, sem salto futuro)"
        },
        {
            "id": 7,
            "title": "Caso 7: Troca de canal próxima ao wrap de 33 bits",
            "v_in": 50000,
            "a_in": 45000,
            "initial_base": WRAP33_MOD - 100000,  # ~2^33 - 100.000 ticks
            "desc": "Timeline anterior em 2^33 - 100k ticks",
            "expected_policy": "Wrap modular contínuo sem underflow"
        }
    ]

    header_fmt = "{:<8} | {:<12} | {:<12} | {:<14} | {:<14} | {:<10} | {:<12} | {:<10}"
    print(header_fmt.format("Caso", "Input Delta", "Output Delta", "Video PTS (s)", "Audio PTS (s)", "PCR (s)", "Monotônico", "Política"))
    print("-" * 105)

    results = []

    for c in cases:
        initial_base = c.get("initial_base", 270000)
        sim = PhaseCoherentRestamperSimulator(initial_max_pts=initial_base)
        sim.start_new_channel()

        # Entrada
        v_in = c["v_in"]
        a_in = c["a_in"]
        delta_in_ticks = signed_diff_33(a_in, v_in)
        delta_in_s = delta_in_ticks / TICKS_PER_SEC

        # Processamento sequencial (Vídeo chega primeiro, depois Áudio)
        v_out = sim.process_video_pts(v_in)
        pcr_out = v_out  # PCR acompanha vídeo
        a_out = sim.process_audio_pts(a_in)

        # Segundo frame para checar monotonicidade
        v_out2 = sim.process_video_pts(v_in + 3000)
        a_out2 = sim.process_audio_pts(a_in + 2880)

        delta_out_ticks = signed_diff_33(a_out, v_out)
        delta_out_s = delta_out_ticks / TICKS_PER_SEC

        is_monotonic_v = signed_diff_33(v_out2, v_out) > 0
        is_monotonic_a = signed_diff_33(a_out2, a_out) > 0
        is_monotonic = is_monotonic_v and is_monotonic_a

        policy_str = "Realigned" if sim.phase_realignment_triggered else "Preserved"

        row = header_fmt.format(
            f"Caso {c['id']}",
            f"{delta_in_s:+.3f}s",
            f"{delta_out_s:+.3f}s",
            f"{v_out/TICKS_PER_SEC:.3f}",
            f"{a_out/TICKS_PER_SEC:.3f}",
            f"{pcr_out/TICKS_PER_SEC:.3f}",
            "SIM" if is_monotonic else "NÃO",
            policy_str
        )
        print(row)

        results.append({
            "case": c,
            "delta_in_s": delta_in_s,
            "delta_out_s": delta_out_s,
            "v_out": v_out,
            "a_out": a_out,
            "is_monotonic": is_monotonic,
            "policy": policy_str
        })

    print("-" * 105)
    print("\nANÁLISE DETALHADA POR CASO:")
    for r in results:
        c = r["case"]
        print(f"\n[{c['title']}]")
        print(f"  Entrada:          Video={c['v_in']} | Audio={c['a_in']} | Delta={r['delta_in_s']:+.3f}s")
        print(f"  Saída:            Video={r['v_out']} ({r['v_out']/90000:.3f}s) | Audio={r['a_out']} ({r['a_out']/90000:.3f}s)")
        print(f"  Delta de Saída:   {r['delta_out_s']:+.3f}s ({r['delta_out_s']*1000:+.1f} ms)")
        print(f"  Política:         {r['policy']} ({c['expected_policy']})")
        print(f"  Monotonicidade:   {'Aprovada' if r['is_monotonic'] else 'Reprovada'}")
        
        # Validação formal de cada caso
        if c["id"] in (1, 2, 3):
            # Deve preservar o delta original
            assert abs(r["delta_out_s"] - r["delta_in_s"]) < 0.001, f"Falha Caso {c['id']}: delta não preservado"
        elif c["id"] in (4, 5, 6):
            # Deve realinhar a fase para perto de 0 (dentro de 1 frame de áudio, ~32ms)
            assert abs(r["delta_out_s"]) <= 0.050, f"Falha Caso {c['id']}: delta não realinhado"
            # No caso 5 (incidente real), comprova ausência do bug de 26.5h
            assert r["a_out"] < 8000000000, "Falha Caso 5: wrap de 26.5h ainda presente!"
        elif c["id"] == 7:
            # Caso 7: wrap modular suave
            assert r["v_out"] < 100000 or r["v_out"] > (WRAP33_MOD - 150000), "Falha Caso 7: wrap modular inconsistente"

    print("\n" + "=" * 105)
    print("✅ TODAS AS ASSERÇÕES DA SIMULAÇÃO PASSARAM COM SUCESSO!")
    print("   Nenhum timestamp negativo produzido.")
    print("   Nenhum wrap artificial para 26.5 horas.")
    print("   Preservação perfeita de interleaving legítimo (Casos 1, 2, 3).")
    print("   Neutralização segura de descontinuidades patológicas (Casos 4, 5, 6).")
    print("   Continuidade em wrap de 33 bits garantida (Caso 7).")
    print("=" * 105)

if __name__ == "__main__":
    run_simulation()
