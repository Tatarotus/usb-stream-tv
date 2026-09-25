#!/usr/bin/env python3
"""
USB Stream TV - High-Performance Multi-Channel Hub & Remote
Servidor central de streaming com troca dinâmica de canal em tempo real,
normalização de áudio/vídeo e controle remoto web mobile-first.
"""

import sys
import os
import time
import subprocess
import threading
import queue
import urllib.parse
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
import json
import re
import hashlib
import shutil

HOST = "0.0.0.0"
PORT = int(os.environ.get("PORT", 8080))
AUTH_PIN = os.environ.get("TV_PIN", "1233")
STANDBY_TIMEOUT = float(os.environ.get("STANDBY_TIMEOUT", 90.0))
XTREAM_UPSTREAM = os.environ.get("XTREAM_UPSTREAM", "http://studut.shop:80")
XTREAM_STREAM_RE = re.compile(r'^/(?:(live|movie|series)/)?([^/]+)/([^/]+)/(\d+)(?:\.([a-zA-Z0-9]+))?$')
XTREAM_CACHE = {}
XTREAM_CACHE_LOCK = threading.Lock()

RESIDENTIAL_HTTP_PROXY = os.environ.get("RESIDENTIAL_HTTP_PROXY", "http://172.20.0.1:8118")
RESIDENTIAL_SOCKS_PROXY = os.environ.get("RESIDENTIAL_SOCKS_PROXY", "socks5h://172.20.0.1:1080")
STREAM_RESOLUTION = os.environ.get("STREAM_RESOLUTION", "1080p").lower()
SLATE_GAP_THRESHOLD = float(os.environ.get("SLATE_GAP_THRESHOLD", 3.0))
SLATE_AUTO_SWITCH_TIMEOUT = float(os.environ.get("SLATE_AUTO_SWITCH_TIMEOUT", 0))


CONFIG_DIR = os.path.dirname(os.path.abspath(__file__))
DEPLOY_FILE = os.path.join(CONFIG_DIR, "channels_deploy.json")
CHANNELS_FILE = os.environ.get("CHANNELS_FILE", DEPLOY_FILE if os.path.exists(DEPLOY_FILE) else os.path.join(CONFIG_DIR, "channels.json"))
MOVIES_DIR = os.environ.get("MOVIES_DIR", os.path.join(CONFIG_DIR, "filmes"))
EVENT_LOG = os.path.join(CONFIG_DIR, "server_events.log")
VOD_DIR = os.environ.get("VOD_DIR", os.path.join(CONFIG_DIR, "vod_cache"))
os.makedirs(VOD_DIR, exist_ok=True)
VOD_TASKS = {}
VOD_TASKS_LOCK = threading.Lock()
ACTIVE_VOD_TASK = None
VOD_META_FILE = os.path.join(VOD_DIR, "vod_meta.json")

def load_vod_tasks():
    global VOD_TASKS
    if os.path.exists(VOD_META_FILE):
        try:
            with open(VOD_META_FILE, "r") as f:
                VOD_TASKS = json.load(f)
        except Exception:
            VOD_TASKS = {}

def save_vod_tasks():
    try:
        with open(VOD_META_FILE, "w") as f:
            json.dump(VOD_TASKS, f, indent=2)
    except Exception:
        pass

load_vod_tasks()

try:
    import gen_template
except Exception:
    gen_template = None


def log_event(msg):
    """Persistent event log: switches, ffmpeg (re)starts, reconnects.
    The background-shell stdout is not captured, so this file is the
    only timeline for correlating TV glitches with server events."""
    try:
        with open(EVENT_LOG, "a") as f:
            f.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")
    except Exception:
        pass

# Grupos inteligentes para a interface do controle remoto
SMART_GROUPS = [
    {"id": "fav", "name": "Favoritos", "icon": "⭐", "filter": "favorites"},
    {"id": "all", "name": "Todos", "icon": "📺", "filter": "all"},
    {"id": "globos", "name": "Globos", "icon": "🌐", "categories": ["GLOBOS"]},
    {"id": "abertos", "name": "TV Aberta", "icon": "📡", "categories": ["TV Aberta", "ABERTOS", "SBT", "RECORD", "BAND"]},
    {"id": "noticias", "name": "Notícias", "icon": "📰", "categories": ["Notícias", "NEWS"]},
    {"id": "esportes", "name": "Esportes", "icon": "⚽", "categories": ["Esportes", "SPORTV"]},
    {"id": "hbo", "name": "HBO", "icon": "🎭", "categories": ["HBO"]},
    {"id": "telecine", "name": "Telecine", "icon": "🍿", "categories": ["TELECINE"]},
    {"id": "filmes_locais", "name": "Meus Filmes", "icon": "🎬", "categories": ["Meus Filmes"]}
]

DEFAULT_CHANNELS = {
    "band-rio": {
        "id": "band-rio",
        "name": "Band Rio",
        "quality": "1080p",
        "category": "TV Aberta",
        "url": "http://studut.shop:80/live/0939303360/3811610453/207177.m3u8",
        "logo": "📺"
    },
    "globo-rj": {
        "id": "globo-rj",
        "name": "Globo RJ",
        "quality": "1080p",
        "category": "TV Aberta",
        "url": "http://studut.shop:80/live/0939303360/3811610453/10006.m3u8",
        "logo": "🌐"
    }
}

class ChannelManager:
    """Gerencia o carregamento dinâmico de canais e cache com base no mtime."""
    def __init__(self):
        self.lock = threading.Lock()
        self.last_mtime = 0
        self.channels = {}
        self.reload()

    def reload(self):
        with self.lock:
            target_file = CHANNELS_FILE
            if not os.path.exists(target_file):
                alt = os.path.join(CONFIG_DIR, "channels_deploy.json")
                if os.path.exists(alt):
                    target_file = alt

            if os.path.exists(target_file):
                try:
                    mtime = os.path.getmtime(target_file)
                    if mtime != self.last_mtime:
                        with open(target_file, "r", encoding="utf-8") as f:
                            raw = json.load(f)

                        parsed = dict(DEFAULT_CHANNELS)
                        base_file = os.path.join(CONFIG_DIR, "channels.json")
                        if os.path.exists(base_file):
                            try:
                                with open(base_file, "r", encoding="utf-8") as bf:
                                    b_raw = json.load(bf)
                                    if isinstance(b_raw, dict):
                                        parsed.update(b_raw)
                            except Exception:
                                pass

                        if isinstance(raw, dict) and "canais" in raw:
                            import re
                            for c in raw.get("canais", []):
                                cid = str(c.get("stream_id") or c.get("nome"))
                                clean_id = re.sub(r"[^a-z0-9_-]", "", c.get("nome", cid).lower().replace(" ", "-"))
                                parsed[clean_id] = {
                                    "id": clean_id,
                                    "name": c.get("nome"),
                                    "quality": "1080p" if c.get("resolucao") in ("FHD", "1080p") else c.get("resolucao", "720p"),
                                    "category": c.get("categoria_nome", "Geral"),
                                    "url": c.get("url_m3u8") or c.get("url_ts"),
                                    "logo": c.get("logo", "📺"),
                                    "description": c.get("nome_original", "")
                                }
                        elif isinstance(raw, dict):
                            parsed.update(raw)

                        # Escaneia pasta de filmes locais se existir
                        if os.path.exists(MOVIES_DIR):
                            for fname in sorted(os.listdir(MOVIES_DIR)):
                                if fname.lower().endswith((".mkv", ".mp4", ".avi", ".ts")):
                                    clean_mid = f"movie-{fname.lower().replace(' ', '-')}"
                                    fpath = os.path.join(MOVIES_DIR, fname)
                                    parsed[clean_mid] = {
                                        "id": clean_mid,
                                        "name": os.path.splitext(fname)[0],
                                        "quality": "1080p",
                                        "category": "Meus Filmes",
                                        "url": fpath,
                                        "logo": "🎬",
                                        "description": f"Filme Pessoal ({fname})"
                                    }

                        self.channels = parsed
                        self.last_mtime = mtime
                        print(f"[✓] Grade recarregada: {len(self.channels)} canais/filmes disponíveis.")
                        return True
                except Exception as e:
                    print(f"[!] Erro ao carregar canais: {e}")
            if not self.channels:
                self.channels = DEFAULT_CHANNELS.copy()
            return False

    def get_all(self):
        self.reload()
        with self.lock:
            return self.channels

    def get_channel(self, cid):
        self.reload()
        with self.lock:
            return self.channels.get(cid)

CH_MGR = ChannelManager()

# Cache de logos em memória para evitar requests repetidos
LOGO_CACHE = {}
LOGO_CACHE_LOCK = threading.Lock()

def encode_ts_timestamp(val, header_bits):
    val = int(val) & 0x1FFFFFFFF
    b0 = (header_bits << 4) | (((val >> 30) & 0x07) << 1) | 1
    b1 = (val >> 22) & 0xFF
    b2 = (((val >> 15) & 0x7F) << 1) | 1
    b3 = (val >> 7) & 0xFF
    b4 = ((val & 0x7F) << 1) | 1
    return bytes([b0, b1, b2, b3, b4])

def encode_pcr(pcr_val):
    base = (pcr_val // 300) & 0x1FFFFFFFF
    ext = pcr_val % 300
    b0 = (base >> 25) & 0xFF
    b1 = (base >> 17) & 0xFF
    b2 = (base >> 9) & 0xFF
    b3 = (base >> 1) & 0xFF
    b4 = ((base & 1) << 7) | 0x7E | ((ext >> 8) & 1)
    b5 = ext & 0xFF
    return bytes([b0, b1, b2, b3, b4, b5])

WRAP33_MOD = 1 << 33  # 8589934592 (limite de rollover de 33 bits do PTS/DTS em 90kHz)
HALF_WRAP33 = 1 << 32  # 4294967296 (ponto de corte para signed delta circular)

# O PCR MPEG-TS é composto por PCR_base (33 bits em 90kHz) e PCR_ext (9 bits em 27MHz, 0..299):
# PCR = PCR_base * 300 + PCR_ext.
# O ciclo completo do PCR_base corresponde a 2^33 ticks de 90kHz, ou seja, (2^33 * 300) ticks de 27MHz (~2.576.980.377.600 ticks).
# PCR_MOD representa este período exato de repetição do PCR_base na escala de 27MHz,
# garantindo que o rollover modular do PCR_base e do PTS ocorram em sincronia perfeita no espaço modular.
PCR_MOD = (1 << 33) * 300  # 2576980377600 ticks a 27MHz

# Operational A/V phase threshold (500 ms in 90kHz ticks).
# Not an MPEG-TS spec or Samsung hardware spec, but our pipeline's operational
# rule to distinguish normal GOP interleaving from upstream phase discontinuities.
OPERATIONAL_MAX_SKEW_TICKS = 45000  # 500 ms

def wrap33(v):
    """33-bit timestamp normalization in the ring [0, 2^33 - 1].
    Strict modular ring arithmetic mod 2^33 is enforced without ad-hoc window clamps."""
    return int(v) % WRAP33_MOD

def wrap46(v):
    """
    Normalização modular do PCR a 27MHz no período de repetição do PCR_base: [0, 2^33 * 300 - 1].
    Preserva a relação síncrona com o espaço modular de 33 bits do PTS (onde 1 tick PTS = 300 ticks PCR).
    """
    return int(v) % PCR_MOD

def signed_diff_33(a, b):
    """
    Calculates modular signed difference (a - b) in the 33-bit ring [-2^32, 2^32 - 1].
    Used to safely compare timestamps across 33-bit rollover boundaries without naive integer subtraction.
    """
    diff = (int(a) - int(b)) % WRAP33_MOD
    if diff >= HALF_WRAP33:
        diff -= WRAP33_MOD
    return diff

class SeamlessRestamper:
    """
    Normalizador de fluxo MPEG-TS em tempo real com preservação de fase A/V e relógio mestre.
    Garante que timestamps (PTS, DTS, PCR) e contadores de continuidade (CC)
    avancem estritamente de forma contínua e crescente mesmo durante a troca de canais,
    sem salto temporal para trás, sem desync de áudio AC3 e preservando a ordem dos B-frames.
    O fluxo de vídeo é o relógio mestre (master clock); o áudio preserva a relação temporal original
    se o skew estiver dentro do limiar operacional (|Δ| <= 500ms), ou realinha para uma nova fase
    coerente caso o upstream apresente descontinuidade patológica (|Δ| > 500ms).
    """
    def __init__(self):
        self.rem = bytearray()
        self.video_pts_offset = None
        self.audio_pts_offset = None
        self.pts_offset = None  # Alias para video_pts_offset (compatibilidade)
        self.max_pts_seen = 90000
        self.first_video_in_pts = None
        self.first_audio_in_pts = None
        self.target_base_pts = 270000
        self.cc_map = {}
        self.last_out_video_pts = None
        self.prev_in_video_pts = None
        self.last_out_audio_pts = None

    def reset_epoch(self):
        self.first_video_in_pts = None
        self.first_audio_in_pts = None
        self.target_base_pts = 270000
        self.max_pts_seen = 90000
        self.last_out_video_pts = None
        self.prev_in_video_pts = None
        self.last_out_audio_pts = None
        self.video_pts_offset = None
        self.audio_pts_offset = None
        self.pts_offset = None
        log_event("PTS_EPOCH_RESET (target_base_pts=270000)")

    def start_new_channel(self):
        self.first_video_in_pts = None
        self.first_audio_in_pts = None
        self.target_base_pts = wrap33(self.max_pts_seen + 3000)
        self.last_out_video_pts = None
        self.prev_in_video_pts = None
        self.last_out_audio_pts = None
        self.video_pts_offset = None
        self.audio_pts_offset = None
        self.pts_offset = None

    def process_chunk(self, data):
        if not data:
            return b""
        if self.rem:
            data = bytes(self.rem) + data
            self.rem.clear()

        n_pkts = len(data) // 188
        rem_len = len(data) % 188
        if rem_len > 0:
            self.rem = bytearray(data[n_pkts * 188:])
            data = data[:n_pkts * 188]

        out = bytearray(data)
        l = len(out)
        for i in range(0, l, 188):
            if out[i] != 0x47:
                continue
            pid = ((out[i+1] & 0x1F) << 8) | out[i+2]
            pusi = (out[i+1] & 0x40) >> 6
            afc = (out[i+3] & 0x30) >> 4

            # Atualiza Continuity Counter para ser contínuo por PID
            if afc in (1, 3):
                if pid not in self.cc_map:
                    self.cc_map[pid] = out[i+3] & 0x0F
                else:
                    self.cc_map[pid] = (self.cc_map[pid] + 1) & 0x0F
                    out[i+3] = (out[i+3] & 0xF0) | self.cc_map[pid]

            offset = 4
            has_af = afc in (2, 3)
            has_payload = afc in (1, 3)

            # Normalização de PCR (Clock de referência do hardware a 27MHz)
            if has_af:
                af_len = out[i+4]
                offset += 1 + af_len
                if af_len >= 7 and (out[i+5] & 0x10):
                    pcr_bytes = out[i+6:i+12]
                    base = (pcr_bytes[0] << 25) | (pcr_bytes[1] << 17) | (pcr_bytes[2] << 9) | (pcr_bytes[3] << 1) | (pcr_bytes[4] >> 7)
                    ext = ((pcr_bytes[4] & 0x01) << 8) | pcr_bytes[5]
                    in_pcr = base * 300 + ext
                    if self.video_pts_offset is None:
                        self.video_pts_offset = wrap33(self.target_base_pts - base)
                        self.pts_offset = self.video_pts_offset
                    out_pcr = wrap46(in_pcr + (self.video_pts_offset * 300))
                    out[i+6:i+12] = encode_pcr(out_pcr)

            # Normalização de PTS e DTS (Áudio MPEG 0xC0-DF, Vídeo 0xE0-EF, Áudio AC3 Dolby 0xBD)
            if has_payload and pusi and offset + 9 <= 188:
                if out[i+offset:i+offset+3] == b'\x00\x00\x01':
                    sid = out[i+offset+3]
                    is_video = (0xE0 <= sid <= 0xEF)
                    is_audio = (0xC0 <= sid <= 0xDF) or (sid == 0xBD)
                    if is_video or is_audio:
                        flags2 = out[i+offset+7]
                        pts_flag = (flags2 & 0x80) >> 7
                        dts_flag = (flags2 & 0x40) >> 6
                        p_pos = i + offset + 9
                        if pts_flag and p_pos + 5 <= i + 188:
                            b = out[p_pos:p_pos+5]
                            in_pts = (((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1))
                            


                            if is_video:
                                if self.first_video_in_pts is None:
                                    self.first_video_in_pts = in_pts
                                    if self.video_pts_offset is None:
                                        self.video_pts_offset = wrap33(self.target_base_pts - in_pts)
                                        self.pts_offset = self.video_pts_offset
                                    if self.first_audio_in_pts is not None:
                                        skew_in = signed_diff_33(self.first_audio_in_pts, in_pts)
                                        if abs(skew_in) <= OPERATIONAL_MAX_SKEW_TICKS:
                                            self.audio_pts_offset = self.video_pts_offset

                                out_pts = wrap33(in_pts + self.video_pts_offset)
                                if self.prev_in_video_pts is not None:
                                    dra = signed_diff_33(in_pts, self.prev_in_video_pts)
                                    if abs(dra) > (1 << 31):
                                        base_ref = self.last_out_video_pts if self.last_out_video_pts is not None else self.target_base_pts
                                        self.video_pts_offset = wrap33(base_ref + 3000 - in_pts)
                                        self.pts_offset = self.video_pts_offset
                                        out_pts = wrap33(in_pts + self.video_pts_offset)
                                        log_event(f"PTS_REBASE dra={dra/90000:+.0f}s")
                                self.prev_in_video_pts = in_pts

                                if self.last_out_video_pts is not None:
                                    jump = signed_diff_33(out_pts, self.last_out_video_pts)
                                    if jump < -45000 or jump > 5400000:
                                        log_event(f"PTS_JUMP {jump/90000:+.1f}s (out_pts={out_pts})")
                                    if jump < -90000 or jump > 450000:
                                        # Re-ancora diretamente em in_pts sem absorver artefatos de wrap 33-bit
                                        self.video_pts_offset = wrap33(self.last_out_video_pts + 3000 - in_pts)
                                        self.pts_offset = self.video_pts_offset
                                        out_pts = wrap33(in_pts + self.video_pts_offset)

                                self.last_out_video_pts = out_pts
                                if signed_diff_33(out_pts, self.max_pts_seen) > 0:
                                    self.max_pts_seen = out_pts

                                out[p_pos:p_pos+5] = encode_ts_timestamp(out_pts, 3 if dts_flag else 2)

                                if dts_flag and p_pos + 10 <= i + 188:
                                    b = out[p_pos+5:p_pos+10]
                                    in_dts = (((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1))
                                    out_dts = wrap33(in_dts + self.video_pts_offset)
                                    out[p_pos+5:p_pos+10] = encode_ts_timestamp(out_dts, 1)

                            elif is_audio:
                                # Relógio mestre é o vídeo
                                if self.audio_pts_offset is None:
                                    self.first_audio_in_pts = in_pts
                                    if self.first_video_in_pts is not None and self.video_pts_offset is not None:
                                        skew_in = signed_diff_33(in_pts, self.first_video_in_pts)
                                        if abs(skew_in) <= OPERATIONAL_MAX_SKEW_TICKS:
                                            # Passo B2: Skew dentro do limiar operacional (<= 500ms)
                                            # Preserva a relação temporal original de upstream
                                            self.audio_pts_offset = self.video_pts_offset
                                        else:
                                            # Passo B3: Descontinuidade de fase do upstream (|skew| > 500ms)
                                            # Inicia nova fase A/V coerente ancorada ao target_base_pts
                                            self.audio_pts_offset = wrap33(self.target_base_pts - in_pts)
                                            log_event(f"A/V_PHASE_REALIGN: in_skew={skew_in/90000:+.3f}s -> realigned audio to target_base_pts")
                                    else:
                                        # Áudio chegou antes do vídeo nesta época: ancora provisoriamente
                                        self.audio_pts_offset = wrap33(self.target_base_pts - in_pts)

                                out_pts = wrap33(in_pts + self.audio_pts_offset)

                                # Proteção contínua de integridade de skew em mid-stream:
                                if self.last_out_video_pts is not None:
                                    curr_skew = signed_diff_33(out_pts, self.last_out_video_pts)
                                    if abs(curr_skew) > OPERATIONAL_MAX_SKEW_TICKS:
                                        self.audio_pts_offset = wrap33(self.last_out_video_pts - in_pts)
                                        out_pts = wrap33(in_pts + self.audio_pts_offset)
                                        log_event(f"A/V_MIDSTREAM_REALIGN: curr_skew={curr_skew/90000:+.3f}s -> realigned audio")

                                # Proteção estrita de monotonicidade com signed_diff_33
                                if self.last_out_audio_pts is not None:
                                    diff_last = signed_diff_33(out_pts, self.last_out_audio_pts)
                                    if diff_last <= 0:
                                        out_pts = wrap33(self.last_out_audio_pts + 2880)

                                if signed_diff_33(out_pts, self.max_pts_seen) > 0:
                                    self.max_pts_seen = out_pts

                                self.last_out_audio_pts = out_pts
                                out[p_pos:p_pos+5] = encode_ts_timestamp(out_pts, 3 if dts_flag else 2)

                                if dts_flag and p_pos + 10 <= i + 188:
                                    b = out[p_pos+5:p_pos+10]
                                    in_dts = (((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1))
                                    out_dts = wrap33(in_dts + self.audio_pts_offset)
                                    diff_dts = signed_diff_33(out_dts, self.last_out_audio_pts)
                                    if diff_dts <= 0:
                                        out_dts = out_pts
                                    out[p_pos+5:p_pos+10] = encode_ts_timestamp(out_dts, 1)

        return bytes(out)

def resolve_youtube(yt_url):
    """
    Usa yt-dlp para extrair título, duração, thumbnail e URLs de stream (vídeo + áudio DASH).
    Tenta primeiro conexão direta para máxima velocidade. Se a VPS for sinalizada como bot,
    faz fallback transparente através do proxy residencial móvel SOCKS5 (Xiaomi Mi A2).
    Retorna dict com metadados estruturados ou levanta ValueError.
    """
    qjs_path = "/usr/bin/qjs"
    base_cmd = [
        "yt-dlp",
        "--no-warnings",
        "--no-playlist",
    ]
    if os.path.exists(qjs_path):
        base_cmd.extend(["--js-runtimes", f"quickjs:{qjs_path}"])
    elif shutil.which("qjs"):
        base_cmd.extend(["--js-runtimes", f"quickjs:{shutil.which('qjs')}"])

    base_cmd.extend([
        "--extractor-args", "youtube:player_client=android,web",
        "-f", "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best",
        "-J",
        yt_url
    ])

    # 1. Tentativa Direta (rápida)
    res = None
    try:
        res = subprocess.run(base_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
    except Exception:
        pass

    # 2. Se falhar ou for bloqueado por detecção de bot de datacenter, tenta via proxy residencial
    need_proxy = False
    used_proxy = False
    if not res or res.returncode != 0:
        need_proxy = True
    elif res.returncode == 0:
        try:
            test_data = json.loads(res.stdout)
            if not test_data.get("url") and not test_data.get("requested_formats"):
                need_proxy = True
        except Exception:
            need_proxy = True

    if need_proxy and RESIDENTIAL_SOCKS_PROXY:
        print("[*] yt-dlp usando proxy residencial de contingência...")
        used_proxy = True
        proxy_clean = RESIDENTIAL_SOCKS_PROXY.replace("socks5h://", "socks5://")
        proxy_cmd = [base_cmd[0], "--proxy", proxy_clean] + base_cmd[1:]
        try:
            res = subprocess.run(proxy_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=35)
        except subprocess.TimeoutExpired:
            raise ValueError("Tempo esgotado ao buscar informações do vídeo no YouTube (timeout 35s).")

    if not res or res.returncode != 0:
        err = res.stderr.strip() if res else "Erro desconhecido"
        err_short = err.split("\n")[-1] if err else f"Código de saída {res.returncode if res else 'None'}"
        raise ValueError(f"yt-dlp: {err_short}")

    try:
        data = json.loads(res.stdout)
    except Exception as e:
        raise ValueError(f"Erro ao decodificar metadados do YouTube: {e}")

    title = data.get("title", "Vídeo do YouTube")
    duration = data.get("duration")
    is_live = bool(data.get("is_live", False))
    thumbnail = data.get("thumbnail", "")

    video_url = None
    audio_url = None

    req_formats = data.get("requested_formats") or []
    if req_formats:
        for f in req_formats:
            if f.get("vcodec") != "none" and not video_url:
                video_url = f.get("url")
            elif f.get("acodec") != "none" and not audio_url:
                audio_url = f.get("url")

    if not video_url:
        video_url = data.get("url")

    if not video_url:
        raise ValueError("Não foi possível extrair URL do fluxo de vídeo do YouTube.")

    return {
        "title": title,
        "duration": duration,
        "is_live": is_live,
        "thumbnail": thumbnail,
        "video_url": video_url,
        "audio_url": audio_url,
        "original_url": yt_url,
        "use_proxy": used_proxy
    }

def build_ffmpeg_cmd(url, audio_url=None, is_live=False, use_proxy=False, start_sec=0):
    is_http = url.startswith("http://") or url.startswith("https://")
    url_lower = url.lower()
    is_googlevideo = "googlevideo" in url_lower or (audio_url and "googlevideo" in audio_url.lower())
    is_vod = any(x in url_lower for x in [
        "fontedecanais", "/movie/", "/series/", "movies/", "series/",
        ".mp4", ".mkv", "googlevideo.com"
    ])
    if is_live:
        is_vod = False
    elif audio_url or is_googlevideo:
        is_vod = True

    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "warning"
    ]

    # Input 0: Vídeo principal com pacing 1.0x em tempo real (-re)
    # Impede que FFmpeg transcode a velocidades excessivas e estoure o buffer de 32MB da TV
    cmd.append("-re")

    if is_http:
        ua = "Mozilla/5.0" if ("studut.shop" in url or "m3u8" in url or is_googlevideo) else "IPTVSmartersPro"
        cmd.extend(["-user_agent", ua])

        # Proxy residencial APENAS para IPTV com bloqueio Cloudflare, NUNCA para googlevideo
        if not is_googlevideo and (use_proxy or is_vod) and RESIDENTIAL_HTTP_PROXY:
            cmd.extend(["-http_proxy", RESIDENTIAL_HTTP_PROXY])

        if ".mp4" in url_lower or ".mkv" in url_lower or is_googlevideo:
            cmd.extend([
                "-rw_timeout", "10000000",
                "-reconnect", "1",
                "-reconnect_streamed", "1",
                "-reconnect_delay_max", "5"
            ])
        else:
            cmd.extend([
                "-rw_timeout", "10000000",
                "-allowed_segment_extensions", "ALL",
                "-extension_picky", "0",
                "-reconnect", "1", "-reconnect_streamed", "1",
                "-reconnect_delay_max", "3"
            ])
    else:
        # Loop local video files infinitely so tests never exhaust the source
        cmd.extend(["-stream_loop", "-1"])

    cmd.extend([
        "-probesize", "1000000",
        "-analyzeduration", "2000000"
    ])
    if start_sec and start_sec > 0:
        cmd.extend(["-ss", str(int(start_sec))])
    cmd.extend(["-i", url])

    # Input 1: Áudio DASH separado (YouTube 1080p)
    if audio_url:
        cmd.extend([
            "-rw_timeout", "10000000",
            "-user_agent", "Mozilla/5.0",
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_delay_max", "5",
            "-probesize", "1000000",
            "-analyzeduration", "2000000"
        ])
        if not is_googlevideo and (use_proxy or is_vod) and RESIDENTIAL_HTTP_PROXY:
            cmd.extend(["-http_proxy", RESIDENTIAL_HTTP_PROXY])
        if start_sec and start_sec > 0:
            cmd.extend(["-ss", str(int(start_sec))])
        cmd.extend(["-i", audio_url])

    if STREAM_RESOLUTION == "1080p":
        vf = "scale=1920:1080:force_original_aspect_ratio=decrease:flags=bicubic,pad=1920:1080:(ow-iw)/2:(oh-ih)/2"
        b_v = "3800k"
        maxrate = "4500k"
        bufsize = "7600k"
    else:
        vf = "scale=1280:720:force_original_aspect_ratio=decrease:flags=bicubic,pad=1280:720:(ow-iw)/2:(oh-ih)/2"
        b_v = "2200k"
        maxrate = "2600k"
        bufsize = "4400k"

    cmd.extend([
        "-map", "0:v:0",
        "-map", "1:a:0" if audio_url else "0:a:0?",
        # Normalização visual: 1080p ou 720p 30fps para Samsung Plasma PL51F4000
        "-vf", vf,
        "-r", "30",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-threads", "0",
        "-b:v", b_v,
        "-maxrate", maxrate,
        "-bufsize", bufsize,
        "-g", "30",
        "-keyint_min", "30",
        "-sc_threshold", "0",
        "-profile:v", "main",
        "-level", "4.1",
        "-x264-params", "repeat-headers=1:aq-mode=2:aq-strength=1.0",

        # Normalização sonora: AC3 (Dolby Digital) a 48kHz (padrão nativo de TV Samsung)
        "-af", "aresample=async=1000:first_pts=0:min_hard_comp=0.100000",
        "-c:a", "ac3",
        "-b:a", "192k",
        "-ar", "48000",
        "-ac", "2",
        "-avoid_negative_ts", "make_zero",
        "-fflags", "+genpts+discardcorrupt+nobuffer",
        "-flags", "low_delay",
        # PIDs fixos e imutáveis por canal (Video 0x100, Audio 0x101, PMT 0x1000)
        "-streamid", "0:256",
        "-streamid", "1:257",
        "-mpegts_pmt_start_pid", "4096",
        "-mpegts_flags", "+resend_headers+pat_pmt_at_frames",
        "-muxdelay", "0", "-muxpreload", "0",
        "-f", "mpegts",
        "pipe:1"
    ])
    return cmd

class StreamHub:
    """
    Hub de streaming central com arquitetura Make-Before-Break:
    - Um único processo upstream (FFmpeg) ativo por vez economiza CPU e conexões de rede.
    - O restamper SeamlessRestamper garante continuidade estrita de PTS/DTS entre trocas de canal.
    - A transição do canal antigo para o novo só ocorre após o novo canal enviar os primeiros bytes válidos.
    """
    def __init__(self):
        self.lock = threading.Lock()
        self.switch_lock = threading.Lock()
        self.restamper = SeamlessRestamper()
        
        initial_ch = (
            CH_MGR.get_channel("band-rio")
            or CH_MGR.get_channel("globo-rj")
            or CH_MGR.get_channel("test-timer")
            or list(CH_MGR.get_all().values())[0]
        )
        self.current_channel_id = initial_ch.get("id", "band-rio")
        self.current_channel_name = initial_ch.get("name", "Band Rio")
        self.current_url = initial_ch.get("url", "")
        self.current_audio_url = None
        self.current_is_live = False
        self.current_is_temporary = False
        self.fallback_channel = None
        self.last_live_channel = self.current_channel_id
        self.temporary_retries = 0
        self.youtube_meta = None
        
        self.subscribers = set()
        self.proc = None
        self.running = True
        self.switching = False
        self.in_standby = False
        self.idle_since = None
        self.total_bytes = 0
        self.start_time = time.time()
        self.last_chunk_time = time.time()
        self.client_telemetry = {}
        self.telemetry_time = 0
        self.pending_command = None
        self.command_output = None
        self.command_done_event = threading.Event()
        self.tablet_pending_cmd = None
        self.tablet_cmd_res = None
        self.tablet_cmd_event = threading.Event()

        # Slate video: carrega .ts pré-encodado na RAM para injeção durante gaps
        self.slate_chunks = []  # list of (chunk_bytes, duration_secs)
        self.slate_mode = False
        self.slate_start_time = 0
        self.stream_health = "ok"  # "ok", "degraded", "slate"
        self.health_window = []  # list of (timestamp, byte_count)
        self.health_window_size = 5.0  # seconds
        self._load_slate()
        
        # Inicia transmissão do primeiro canal
        self._start_initial()
        
        # Thread de leitura contínua e distribuição
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()
        
        # Thread de keepalive (envia TS NULL packets se o buffer upstream atrasar)
        self.keepalive_thread = threading.Thread(target=self._keepalive_loop, daemon=True)
        self.keepalive_thread.start()

    def _start_initial(self, start_sec=0):
        cmd = build_ffmpeg_cmd(
            self.current_url, 
            audio_url=self.current_audio_url, 
            is_live=self.current_is_live,
            use_proxy=getattr(self, "current_use_proxy", False),
            start_sec=start_sec
        )
        tag = f" (offset {start_sec}s)" if start_sec > 0 else ""
        print(f"[*] Hub iniciando canal: {self.current_channel_name} ({self.current_url}){tag}")
        log_event(f"FFMPEG_START {self.current_channel_id}{tag}")
        try:
            self.proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                bufsize=1048576
            )
        except Exception as e:
            print(f"[!] Erro ao iniciar processo: {e}")

    def _load_slate(self):
        """Carrega o vídeo-placeholder 'Sem Sinal' na RAM, dividido em chunks com pacing exato."""
        slate_file = "slate_1080p.ts" if STREAM_RESOLUTION == "1080p" else "slate.ts"
        slate_path = os.path.join(CONFIG_DIR, slate_file)
        if not os.path.exists(slate_path):
            fallback = "slate.ts" if STREAM_RESOLUTION == "1080p" else "slate_1080p.ts"
            fallback_path = os.path.join(CONFIG_DIR, fallback)
            if os.path.exists(fallback_path):
                slate_path = fallback_path
                slate_file = fallback
            else:
                print(f"[!] Slate não encontrado: {slate_path}. Proteção contra travamento DESABILITADA.")
                return
        try:
            with open(slate_path, "rb") as f:
                raw = f.read()
            total_pkts = len(raw) // 188
            if total_pkts == 0:
                print("[!] Slate vazio ou inválido.")
                return
            pps = total_pkts / 10.0  # slate tem duração de 10s
            chunk_size = 348 * 188  # 65424 bytes
            chunks_raw = [raw[i:i+chunk_size] for i in range(0, len(raw), chunk_size)]
            self.slate_chunks = []
            for c in chunks_raw:
                if len(c) % 188 == 0 and len(c) > 0:
                    dur = (len(c) // 188) / pps
                    self.slate_chunks.append((c, dur))
            total_kb = len(raw) / 1024
            print(f"[✓] Slate carregado: {slate_file} ({total_kb:.0f} KB, {len(self.slate_chunks)} chunks)")
        except Exception as e:
            print(f"[!] Erro ao carregar slate: {e}")

    def get_current_bitrate_kbps(self):
        """Calcula bitrate médio em kbps dos últimos N segundos."""
        now = time.time()
        cutoff = now - self.health_window_size
        with self.lock:
            self.health_window = [(t, b) for t, b in self.health_window if t >= cutoff]
            if not self.health_window:
                return 0.0
            total_bytes = sum(b for _, b in self.health_window)
            span = now - self.health_window[0][0]
            if span <= 0:
                return 0.0
            return (total_bytes * 8) / (span * 1000)

    def reset_pts_epoch(self):
        with self.lock:
            self.restamper.reset_epoch()

    def subscribe(self):
        q = queue.Queue(maxsize=300)
        with self.lock:
            if len(self.subscribers) == 0:
                self.restamper.reset_epoch()
            self.subscribers.add(q)
            self.idle_since = None
            if self.in_standby:
                print("[*] Despertando do Standby Inteligente: TV conectada!")
                log_event("STANDBY_WAKEUP (tv_connected)")
                self.in_standby = False
                self._start_initial()
            print(f"[+] Novo cliente conectado ao Hub. Total de ouvintes: {len(self.subscribers)}")
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.subscribers.discard(q)
            if len(self.subscribers) == 0 and not self.in_standby:
                self.idle_since = time.time()
                print(f"[*] Zero ouvintes. Standby agendado para {int(STANDBY_TIMEOUT)}s...")
            print(f"[-] Cliente desconectado do Hub. Total de ouvintes: {len(self.subscribers)}")

    def _broadcast(self, data):
        if not data:
            return
        for q in list(self.subscribers):
            try:
                q.put_nowait(data)
            except queue.Full:
                # Buffer cheio por lag transitório: descarta o chunk mais antigo
                # para que o leitor avance sem travar, preservando a conexão do assinante!
                try:
                    q.get_nowait()
                except queue.Empty:
                    pass
                try:
                    q.put_nowait(data)
                except queue.Full:
                    pass

    def _handle_proc_exit(self, p, rc):
        if p != self.proc or self.switching or self.in_standby:
            return

        if self.current_is_temporary:
            meta = self.youtube_meta or {}
            duration = meta.get("duration") or 0
            started_at = meta.get("started_at") or time.time()
            elapsed = time.time() - started_at

            # Checa se o vídeo terminou naturalmente (rc == 0 e tempo decorrido próximo à duração total)
            finished_naturally = (rc == 0) and (duration == 0 or elapsed >= max(0, duration - 15))

            if not finished_naturally and self.temporary_retries < 3:
                self.temporary_retries += 1
                seek_pos = max(0, int(elapsed - 2))
                print(f"[!] Transmissão temporária ({self.current_channel_name}) oscilou (rc={rc}, {int(elapsed)}s/{duration}s). Reconectando em {seek_pos}s (tentativa {self.temporary_retries}/3)...")
                log_event(f"TEMPORARY_STREAM_RETRY {self.current_channel_id} (attempt {self.temporary_retries}/3 at {seek_pos}s)")
                with self.lock:
                    if self.slate_chunks:
                        self.restamper.start_new_channel()
                        self.slate_mode = True
                        self.slate_start_time = time.time()
                        self.stream_health = "slate"
                time.sleep(1)
                if not self.switching and not self.in_standby:
                    with self.lock:
                        self.restamper.start_new_channel()
                    self._start_initial(start_sec=seek_pos)
                return

            # Terminou normalmente ou esgotou tentativas de reconexão
            fallback_ch = self.fallback_channel or getattr(self, "last_live_channel", None) or "band-rio"
            ch = CH_MGR.get_channel(fallback_ch)
            reason = "finalizada" if finished_naturally else f"interrompida após {self.temporary_retries} tentativas"
            print(f"[✓] Transmissão temporária {reason} ({self.current_channel_name}, rc={rc}). Retornando à TV ao vivo ({fallback_ch})...")
            log_event(f"TEMPORARY_STREAM_ENDED {self.current_channel_id} -> fallback to {fallback_ch} (reason: {reason})")
            self.current_is_temporary = False
            self.fallback_channel = None
            self.youtube_meta = None
            self.temporary_retries = 0
            if ch:
                self.current_channel_id = ch["id"]
                self.current_channel_name = ch["name"]
                self.current_url = ch["url"]
                self.current_audio_url = None
                self.current_is_live = False
            self.switch_channel(channel_id=fallback_ch, force=True)
        else:
            print(f"[!] Canal {self.current_channel_name} desconectou (rc={rc}). Reconectando...")
            log_event(f"FFMPEG_DIED rc={rc} ch={self.current_channel_id} -> reconnect")
            with self.lock:
                if self.slate_chunks:
                    self.restamper.start_new_channel()
                    self.slate_mode = True
                    self.slate_start_time = time.time()
                    self.stream_health = "slate"
            time.sleep(1)
            if not self.switching and not self.in_standby:
                with self.lock:
                    self.restamper.start_new_channel()
                self._start_initial()

    def _reader_loop(self):
        while self.running:
            # Standby inteligente: 90s sem ouvintes encerra FFmpeg
            if len(self.subscribers) == 0 and not self.in_standby:
                if self.idle_since is None:
                    self.idle_since = time.time()
                elif time.time() - self.idle_since >= STANDBY_TIMEOUT:
                    with self.lock:
                        print(f"[*] Standby Inteligente: Sem ouvintes por {int(STANDBY_TIMEOUT)}s. Encerrando FFmpeg para poupar IPTV.")
                        log_event("STANDBY_ENTER")
                        self.in_standby = True
                        if self.proc:
                            try:
                                self.proc.kill()
                            except Exception:
                                pass
                            self.proc = None
                        continue

            if self.in_standby:
                time.sleep(0.5)
                continue

            p = self.proc
            if not p:
                time.sleep(0.05)
                continue

            if p.poll() is not None:
                self._handle_proc_exit(p, p.poll())
                time.sleep(0.05)
                continue

            try:
                chunk = os.read(p.stdout.fileno(), 65424)
            except Exception:
                chunk = None

            if chunk:
                with self.lock:
                    # Se o processo ativo mudou enquanto lemos, ignora dados do processo antigo
                    if p != self.proc:
                        continue
                    if self.slate_mode:
                        print("[✓] Upstream recuperou — saindo do modo Slate.")
                        log_event("SLATE_EXIT (upstream recovered)")
                        self.restamper.start_new_channel()
                        self.slate_mode = False
                        self.stream_health = "ok"
                    processed = self.restamper.process_chunk(chunk)
                    if processed:
                        self.total_bytes += len(processed)
                        self.last_chunk_time = time.time()
                        self.health_window.append((time.time(), len(processed)))
                        self._broadcast(processed)
            else:
                # Chunk vazio ou erro de leitura: processo pode ter terminado
                if p.poll() is not None:
                    self._handle_proc_exit(p, p.poll())
                time.sleep(0.01)

    def _keepalive_loop(self):
        """Keepalive inteligente com injeção de Slate Video.
        
        Quando FFmpeg para de produzir dados por > SLATE_GAP_THRESHOLD segundos,
        injeta o vídeo 'Sem Sinal' pré-encodado em loop, mantendo o decodificador
        H.264 do ConnectShare ativo e evitando travamento da TV.
        
        Fallback: se o slate não foi carregado, envia NULL packets (comportamento legado).
        """
        null_pkt = b'\x47\x1f\xff\x10' + b'\xff' * 184
        null_burst = null_pkt * 174  # ~32 KB

        slate_idx = 0
        last_slate_broadcast = 0.0
        next_slate_pace = 0.0

        while self.running:
            time.sleep(0.05)
            if len(self.subscribers) == 0 or self.switching or self.in_standby:
                continue

            gap = time.time() - self.last_chunk_time

            if gap <= 0.6:
                # Stream saudável
                if self.slate_mode:
                    print("[✓] Upstream recuperou — saindo do modo Slate.")
                    log_event("SLATE_EXIT (upstream recovered)")
                    with self.lock:
                        self.restamper.start_new_channel()
                    self.slate_mode = False
                    self.stream_health = "ok"
                    slate_idx = 0
                continue

            # Se já estamos em modo Slate (por desconexão ou gap longo anterior),
            # continua transmitindo os chunks do slate compassados até que dados reais voltem!
            if self.slate_mode:
                # Watchdog de recuperação: se estamos em slate há mais de 15s e o processo upstream
                # ainda consta como vivo (ex: socket TCP congelado pela operadora sem EOF),
                # força a finalização do FFmpeg para disparar reconexão imediata!
                if self.slate_start_time and (time.time() - self.slate_start_time > 15.0):
                    p = self.proc
                    if p and p.poll() is None:
                        print("[!] Upstream travado em Slate por >15s. Forçando reinício do FFmpeg...")
                        log_event("WATCHDOG_FFMPEG_KILL (stuck in slate >15s)")
                        try:
                            p.kill()
                        except Exception:
                            pass

                if self.slate_chunks:
                    now = time.time()
                    if now - last_slate_broadcast >= next_slate_pace:
                        chunk, dur = self.slate_chunks[slate_idx % len(self.slate_chunks)]
                        with self.lock:
                            processed = self.restamper.process_chunk(chunk)
                            if processed:
                                self._broadcast(processed)
                        slate_idx += 1
                        last_slate_broadcast = now
                        next_slate_pace = dur
                else:
                    with self.lock:
                        self._broadcast(null_burst)
                continue

            # Gap detectado: decidir entre NULL packets transitórios e Slate
            if gap < SLATE_GAP_THRESHOLD:
                # Gap curto (< 3.0s): NULL packets para absorver jitter transitório
                if self.stream_health == "ok":
                    self.stream_health = "degraded"
                with self.lock:
                    self._broadcast(null_burst)
                continue

            # Gap longo (>= SLATE_GAP_THRESHOLD): modo Slate!
            if self.slate_chunks:
                print(f"[⚠] Gap de {gap:.1f}s detectado — entrando no modo Slate (Sem Sinal).")
                log_event(f"SLATE_ENTER gap={gap:.1f}s")
                with self.lock:
                    self.restamper.start_new_channel()
                self.slate_mode = True
                self.slate_start_time = time.time()
                self.stream_health = "slate"
                slate_idx = 0
                now = time.time()
                chunk, dur = self.slate_chunks[0]
                with self.lock:
                    processed = self.restamper.process_chunk(chunk)
                    if processed:
                        self._broadcast(processed)
                slate_idx = 1
                last_slate_broadcast = now
                next_slate_pace = dur
            else:
                # Fallback sem slate: NULL packets legados
                if self.stream_health != "slate":
                    self.stream_health = "degraded"
                with self.lock:
                    self._broadcast(null_burst)

    def switch_channel(self, channel_id=None, custom_url=None, custom_name=None, force=False):
        if not self.switch_lock.acquire(blocking=True, timeout=4.0):
            print("[~] Troca de canal anterior ainda em andamento. Aguarde...")
            return False

        def _do_switch():
            try:
                self.switching = True
                if custom_url:
                    target_id = f"custom_{custom_url}"
                    target_name = custom_name or "Canal Personalizado"
                    target_url = custom_url
                elif channel_id:
                    ch = CH_MGR.get_channel(channel_id)
                    if not ch:
                        return
                    target_id = ch["id"]
                    target_name = ch["name"]
                    target_url = ch["url"]
                    if not custom_url:
                        self.last_live_channel = target_id
                else:
                    return

                if not force and target_id == self.current_channel_id:
                    print(f"[~] Já sintonizado em: {target_name}")
                    return

                print(f"\n[⚡] INICIANDO SINTONIA MAKE-BEFORE-BREAK: {target_name} ({target_url})")
                log_event(f"SWITCH_BEGIN {self.current_channel_id} -> {target_id}")
                cmd = build_ffmpeg_cmd(target_url)
                try:
                    new_p = subprocess.Popen(
                        cmd,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL,
                        bufsize=1048576
                    )
                except Exception as e:
                    print(f"[!] Erro ao iniciar processo para {target_name}: {e}")
                    return

                # Aguarda os primeiros bytes válidos do novo canal (timeout 22s)
                # O canal anterior CONTINUA TRANSMITINDO durante este tempo!
                first_chunk = []
                def read_first():
                    try:
                        c = os.read(new_p.stdout.fileno(), 65424)
                        if c:
                            first_chunk.append(c)
                    except Exception:
                        pass

                t = threading.Thread(target=read_first, daemon=True)
                t.start()
                t.join(timeout=22.0)

                if not first_chunk or len(first_chunk[0]) == 0:
                    print(f"[!] Timeout ao conectar em {target_name}. Mantendo canal {self.current_channel_name} sem queda.")
                    log_event(f"SWITCH_TIMEOUT {target_id} (kept {self.current_channel_id})")
                    try:
                        new_p.kill()
                    except Exception:
                        pass
                    return

                # Chaveamento atômico instantâneo!
                with self.lock:
                    old_p = self.proc
                    self.proc = new_p
                    self.current_channel_id = target_id
                    self.current_channel_name = target_name
                    self.current_url = target_url
                    self.current_audio_url = None
                    self.current_is_live = False
                    self.current_is_temporary = False
                    self.current_use_proxy = False
                    self.fallback_channel = None
                    self.youtube_meta = None
                    self.temporary_retries = 0

                    # Avança timestamps e continuity counters de forma estritamente contínua
                    self.restamper.start_new_channel()

                    # Transmite o primeiro chunk do novo canal com timestamps contínuos
                    processed = self.restamper.process_chunk(first_chunk[0])
                    if processed:
                        self.total_bytes += len(processed)
                        self.last_chunk_time = time.time()
                        self._broadcast(processed)

                    # Encerra o processo do canal anterior
                    if old_p:
                        try:
                            old_p.kill()
                        except Exception:
                            pass

                print(f"[✓] CHAVEAMENTO CONCLUÍDO COM SUCESSO! Novo canal ativo na TV: {target_name}")
                log_event(f"SWITCH_OK {target_id}")
            finally:
                self.switching = False
                self.switch_lock.release()

        threading.Thread(target=_do_switch, daemon=True).start()
        return True

    def switch_youtube(self, meta):
        """
        Sintoniza vídeo ou live do YouTube de forma Make-Before-Break.
        Suporta dual-stream DASH (vídeo 1080p + áudio separado).
        Ao final de vídeos normais (VOD), retorna automaticamente ao canal de TV padrão.
        """
        if not self.switch_lock.acquire(blocking=True, timeout=4.0):
            print("[~] Troca de transmissão anterior ainda em andamento. Aguarde...")
            return False

        def _do_switch():
            try:
                self.switching = True
                self.temporary_retries = 0
                yt_id = hashlib.md5(meta['original_url'].encode()).hexdigest()[:8]
                target_id = f"youtube_{yt_id}"
                target_name = f"YouTube: {meta['title']}"
                target_url = meta["video_url"]
                audio_url = meta.get("audio_url")
                is_live = meta.get("is_live", False)
                use_proxy = meta.get("use_proxy", False)
                return_channel = (
                    getattr(self, "last_live_channel", None)
                    or (self.current_channel_id if not self.current_is_temporary else None)
                    or self.fallback_channel
                    or "band-rio"
                )

                print(f"\n[▶️] INICIANDO TRANSMISSÃO DO YOUTUBE: {target_name} (via_proxy={use_proxy})")
                log_event(f"YOUTUBE_START {target_id} ({meta['title']})")

                cmd = build_ffmpeg_cmd(target_url, audio_url=audio_url, is_live=is_live, use_proxy=use_proxy)
                try:
                    new_p = subprocess.Popen(
                        cmd,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL,
                        bufsize=1048576
                    )
                except Exception as e:
                    print(f"[!] Erro ao iniciar processo para {target_name}: {e}")
                    return

                # Aguarda os primeiros bytes válidos (timeout 22s para YouTube DASH)
                first_chunk = []
                def read_first():
                    try:
                        c = os.read(new_p.stdout.fileno(), 65424)
                        if c:
                            first_chunk.append(c)
                    except Exception:
                        pass

                t = threading.Thread(target=read_first, daemon=True)
                t.start()
                t.join(timeout=22.0)

                if not first_chunk or len(first_chunk[0]) == 0:
                    print(f"[!] Timeout ao conectar no YouTube: {target_name}. Mantendo canal anterior {self.current_channel_name}.")
                    log_event(f"YOUTUBE_TIMEOUT {target_id} (kept {self.current_channel_id})")
                    try:
                        new_p.kill()
                    except Exception:
                        pass
                    return

                # Chaveamento atômico instantâneo na TV!
                with self.lock:
                    old_p = self.proc
                    self.proc = new_p
                    self.current_channel_id = target_id
                    self.current_channel_name = target_name
                    self.current_url = target_url
                    self.current_audio_url = audio_url
                    self.current_is_live = is_live
                    self.current_is_temporary = not is_live
                    self.current_use_proxy = use_proxy
                    self.fallback_channel = return_channel
                    self.youtube_meta = {
                        "title": meta["title"],
                        "duration": meta.get("duration"),
                        "thumbnail": meta.get("thumbnail"),
                        "original_url": meta["original_url"],
                        "started_at": int(time.time()),
                        "is_live": is_live
                    }

                    # Reinicia restamper de forma suave e contínua
                    self.restamper.start_new_channel()

                    processed = self.restamper.process_chunk(first_chunk[0])
                    if processed:
                        self.total_bytes += len(processed)
                        self.last_chunk_time = time.time()
                        self._broadcast(processed)

                    if old_p:
                        try:
                            old_p.kill()
                        except Exception:
                            pass

                print(f"[✓] YOUTUBE TRANSMITINDO NA TV COM SUCESSO! Novo conteúdo: {target_name}")
                log_event(f"YOUTUBE_OK {target_id}")
            finally:
                self.switching = False
                self.switch_lock.release()

        threading.Thread(target=_do_switch, daemon=True).start()
        return True

HUB = StreamHub()

def dispatch_device_cmd(cmd):
    """Envia comando para o dispositivo ativo (Tablet ou Xiaomi)."""
    log_event(f"DISPATCH_CMD {cmd}")
    print(f"[*] Disparando comando para aparelho: {cmd}")
    HUB.pending_command = f"su -c '{cmd}'"
    HUB.tablet_pending_cmd = cmd

    def _run_adb():
        for port in [25555, 25556]:
            try:
                subprocess.run(["adb", "-s", f"127.0.0.1:{port}", "shell", f"su -c '{cmd}'"],
                               timeout=3, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except Exception:
                pass
    threading.Thread(target=_run_adb, daemon=True).start()

def _prepare_vod_thread(task_id, url, title):
    with VOD_TASKS_LOCK:
        task = VOD_TASKS.get(task_id)
        if not task:
            return
        task["status"] = "processing"
        task["progress"] = 5

    out_file = os.path.join(VOD_DIR, f"{task_id}.mp4")
    out_tmpl = os.path.join(VOD_DIR, f"{task_id}.bin")

    is_yt = any(x in url.lower() for x in ["youtube.com", "youtu.be"])
    clean_title = title or "Filme VOD"
    duration = 0

    try:
        if is_yt:
            raw_file = os.path.join(VOD_DIR, f"{task_id}_raw.mkv")
            if os.path.exists(raw_file) and os.path.getsize(raw_file) > 1000000:
                print(f"[VOD] Arquivo raw já existente ({os.path.getsize(raw_file) / (1024*1024):.1f} MB), iniciando conversão...")
                with VOD_TASKS_LOCK:
                    task["progress"] = 50
                try:
                    probe_out = subprocess.check_output([
                        "ffprobe", "-v", "error", "-show_entries", "format=duration",
                        "-of", "default=noprint_wrappers=1:nokey=1", raw_file
                    ], text=True).strip()
                    duration = float(probe_out)
                except Exception:
                    duration = 600
            else:
                with VOD_TASKS_LOCK:
                    task["progress"] = 10
                try:
                    meta = resolve_youtube(url)
                    clean_title = meta.get("title") or clean_title
                    duration = meta.get("duration") or 0
                except Exception as e:
                    print(f"[*] resolve_youtube informativo falhou: {e}, prosseguindo para download direto...")

                fat_name_preview = re.sub(r'[^a-zA-Z0-9 _-]', '', clean_title).strip()
                fat_name_preview = (fat_name_preview[:26] or "FILME") + ".mp4"
                with VOD_TASKS_LOCK:
                    task["title"] = clean_title
                    task["display_name"] = fat_name_preview
                    save_vod_tasks()

                yt_cmd = [
                    "yt-dlp",
                    "--no-warnings",
                    "--no-playlist",
                    "--extractor-args", "youtube:player_client=android,web",
                    "-f", "bestvideo[height<=720]+bestaudio/best[height<=720]/best",
                    "--merge-output-format", "mkv",
                    "-o", raw_file
                ]
                if os.path.exists("/usr/bin/qjs"):
                    yt_cmd.extend(["--js-runtimes", "quickjs:/usr/bin/qjs"])
                elif shutil.which("qjs"):
                    yt_cmd.extend(["--js-runtimes", f"quickjs:{shutil.which('qjs')}"])
                if RESIDENTIAL_SOCKS_PROXY:
                    proxy_clean = RESIDENTIAL_SOCKS_PROXY.replace("socks5h://", "socks5://")
                    yt_cmd.extend(["--proxy", proxy_clean])
                yt_cmd.append(url)

                print(f"[VOD] Baixando YouTube '{clean_title}'...")
                proc_yt = subprocess.Popen(yt_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                pct_pat = re.compile(r"(\d+\.\d+)%")
                for line in proc_yt.stdout:
                    m = pct_pat.search(line)
                    if m:
                        # Download phase maps to 10% - 50%
                        pct = min(50, max(10, int(float(m.group(1)) * 0.4 + 10)))
                        with VOD_TASKS_LOCK:
                            task["progress"] = pct
                proc_yt.wait()
                if proc_yt.returncode != 0:
                    raise RuntimeError(f"yt-dlp falhou com código {proc_yt.returncode}")

            # Transcode raw media with FFmpeg into 100% Samsung-compatible H.264 + AC3 stereo
            can_copy_video = False
            try:
                probe_cmd = [
                    "ffprobe", "-v", "error", "-select_streams", "v:0",
                    "-show_entries", "stream=codec_name,level,pix_fmt",
                    "-of", "json", raw_file
                ]
                v_meta = json.loads(subprocess.check_output(probe_cmd, text=True))
                st = (v_meta.get("streams") or [{}])[0]
                codec = st.get("codec_name", "").lower()
                pix = st.get("pix_fmt", "").lower()
                lvl = int(st.get("level", 99))
                if codec in ("h264", "avc1") and pix == "yuv420p" and lvl <= 41:
                    can_copy_video = True
            except Exception:
                can_copy_video = False

            if can_copy_video:
                cmd = [
                    "ffmpeg", "-y", "-i", raw_file,
                    "-c:v", "copy",
                    "-c:a", "ac3", "-b:a", "192k", "-ar", "48000", "-ac", "2",
                    "-movflags", "+faststart",
                    "-f", "mp4",
                    out_file
                ]
                print(f"[VOD] Stream copy de vídeo direto + AC3 Samsung (modo ultra-rápido 90x) '{clean_title}'...")
            else:
                cmd = [
                    "ffmpeg", "-y", "-i", raw_file,
                    "-vf", "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2",
                    "-r", "30",
                    "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "main", "-level", "4.1",
                    "-b:v", "2600k", "-maxrate", "3000k", "-bufsize", "1800k", "-g", "30",
                    "-c:a", "ac3", "-b:a", "192k", "-ar", "48000", "-ac", "2",
                    "-movflags", "+faststart",
                    "-f", "mp4",
                    out_file
                ]
                print(f"[VOD] Codificando para Samsung TV H.264/AC3 '{clean_title}'...")
            proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True)
            time_pat = re.compile(r"time=(\d+):(\d+):(\d+\.\d+)")
            err_lines = []
            for line in proc.stderr:
                err_lines.append(line.strip())
                if len(err_lines) > 20:
                    err_lines.pop(0)
                m = time_pat.search(line)
                if m:
                    hrs, mins, secs = int(m.group(1)), int(m.group(2)), float(m.group(3))
                    cur_secs = hrs * 3600 + mins * 60 + secs
                    if duration > 0:
                        pct = min(98, max(50, int(50 + (cur_secs / duration) * 48)))
                    else:
                        pct = min(95, 50 + int(cur_secs / 30))
                    with VOD_TASKS_LOCK:
                        task["progress"] = pct
            proc.wait()
            try:
                if os.path.exists(raw_file):
                    os.remove(raw_file)
            except Exception:
                pass
            if proc.returncode != 0:
                err_snippet = " ".join([l for l in err_lines if "error" in l.lower() or "failed" in l.lower()][-3:])
                raise RuntimeError(f"FFmpeg falhou ({proc.returncode}): {err_snippet or 'erro na conversão'}")
        else:
            use_proxy = any(kw in url.lower() for kw in ["fontedecanais", "movie", "series"])
            cmd = ["ffmpeg", "-y"]
            if use_proxy and RESIDENTIAL_HTTP_PROXY:
                cmd.extend(["-http_proxy", RESIDENTIAL_HTTP_PROXY])
            cmd.extend([
                "-user_agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
                "-i", url,
                "-vf", "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2",
                "-r", "30",
                "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "main", "-level", "4.1",
                "-b:v", "2600k", "-maxrate", "3000k", "-bufsize", "1800k", "-g", "30",
                "-c:a", "ac3", "-b:a", "192k", "-ar", "48000", "-ac", "2",
                "-movflags", "+faststart",
                "-f", "mp4",
                out_file
            ])

            print(f"[VOD] Processando stream '{clean_title}'...")
            proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True)

            time_pat = re.compile(r"time=(\d+):(\d+):(\d+\.\d+)")
            err_lines = []
            for line in proc.stderr:
                err_lines.append(line.strip())
                if len(err_lines) > 20:
                    err_lines.pop(0)
                m = time_pat.search(line)
                if m:
                    hrs, mins, secs = int(m.group(1)), int(m.group(2)), float(m.group(3))
                    cur_secs = hrs * 3600 + mins * 60 + secs
                    if duration > 0:
                        pct = min(98, max(10, int((cur_secs / duration) * 100)))
                    else:
                        pct = min(95, 10 + int(cur_secs / 30))
                    with VOD_TASKS_LOCK:
                        task["progress"] = pct

            proc.wait()
            if proc.returncode != 0:
                err_snippet = " ".join([l for l in err_lines if "error" in l.lower() or "failed" in l.lower()][-3:])
                raise RuntimeError(f"FFmpeg falhou ({proc.returncode}): {err_snippet or 'erro na conversão'}")

        file_size = os.path.getsize(out_file)
        if file_size < 10000:
            raise RuntimeError("Arquivo gerado vazio ou corrompido")

        fat_name = re.sub(r'[^a-zA-Z0-9 _-]', '', clean_title).strip()
        fat_name = (fat_name[:26] or "FILME") + ".mp4"

        if gen_template:
            gen_template.build_fat_template(file_name=fat_name, file_size=file_size, out_path=out_tmpl)

        with VOD_TASKS_LOCK:
            task["status"] = "ready"
            task["progress"] = 100
            task["file_path"] = out_file
            task["template_path"] = out_tmpl
            task["file_size"] = file_size
            task["display_name"] = fat_name
            save_vod_tasks()
        print(f"[✓] VOD pronto: {clean_title} ({file_size / (1024*1024):.1f} MB)")
    except Exception as e:
        print(f"[!] Erro no VOD {task_id}: {e}")
        with VOD_TASKS_LOCK:
            task["status"] = "error"
            task["error"] = str(e)
            save_vod_tasks()

def resume_interrupted_vod_tasks():
    """Retoma automaticamente tarefas VOD que foram interrompidas por reinicialização."""
    with VOD_TASKS_LOCK:
        tasks = list(VOD_TASKS.values())
    for t in tasks:
        if t.get("status") in ("pending", "processing"):
            tid = t.get("id")
            url = t.get("url")
            title = t.get("title")
            if tid and url:
                print(f"[*] Auto-retomando tarefa VOD: {tid} ({title})")
                threading.Thread(target=_prepare_vod_thread, args=(tid, url, title), daemon=True).start()


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

class RequestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Suprime logs de status e logo polling para manter terminal limpo
        msg = format % args
        if "/api/status" in msg or "/api/logo" in msg:
            return
        sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] {self.client_address[0]} - {msg}\n")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Auth-PIN")
        self.end_headers()

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/":
            self.send_dashboard()
        elif path in ("/live.ts", "/stream"):
            self.stream_live_ts()
        elif path in ("/player_api.php", "/xmltv.php"):
            self.proxy_player_api("GET")
        elif XTREAM_STREAM_RE.match(path):
            m = XTREAM_STREAM_RE.match(path)
            kind, user, pwd, stream_id, ext = m.groups()
            self.handle_xtream_stream(kind or "live", user, pwd, stream_id, ext)
        elif path.startswith("/vod/"):
            self.handle_vod_stream(path)
        elif path == "/api/vod/status":
            self.send_vod_status()
        elif path == "/api/status":
            self.send_status_json()
        elif path == "/api/channels":
            self.send_channels_json()
        elif path == "/api/groups":
            self.send_groups_json()
        elif path == "/remote.m3u":
            self.send_remote_m3u()
        elif path.startswith("/remote/"):
            self.handle_remote_play(path)
        elif path == "/api/tablet_cmd":
            cmd = HUB.tablet_pending_cmd or "none"
            HUB.tablet_pending_cmd = None
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(cmd.encode("utf-8"))
        elif path == "/api/logo":
            self.proxy_logo(query.get("url", [""])[0])
        elif path == "/fuse_direct_arm_verified":
            self.send_fuse_bin()
        elif path.endswith(".sh") and not "/" in path[1:]:
            fpath = os.path.join(CONFIG_DIR, path.lstrip("/"))
            if os.path.exists(fpath):
                with open(fpath, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_error(404)
        elif path == "/fat_template.bin":
            fpath = os.path.join(CONFIG_DIR, "fat_template.bin")
            if os.path.exists(fpath):
                with open(fpath, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_error(404)
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/switch":
            self.handle_switch()
        elif path == "/api/vod/prepare":
            self.handle_vod_prepare()
        elif path == "/api/vod/play":
            self.handle_vod_play()
        elif path == "/api/vod/live":
            self.handle_vod_live()
        elif path == "/api/youtube":
            self.handle_youtube()
        elif path in ("/player_api.php", "/xmltv.php"):
            self.proxy_player_api("POST")
        elif path == "/api/sync":
            self.handle_sync()
        elif path == "/api/telemetry":
            self.handle_telemetry()
        elif path == "/api/telemetry_result":
            self.handle_telemetry_result()
        elif path == "/api/exec":
            self.handle_remote_exec()
        elif path == "/api/tablet_cmd_res":
            self.handle_tablet_cmd_res()
        elif path == "/api/tablet_exec":
            self.handle_tablet_exec()
        elif path == "/api/reset_epoch":
            self.handle_reset_epoch()
        else:
            self.send_error(404, "Not Found")

    def proxy_logo(self, url):
        """Proxy seguro de logos para evitar problemas de Mixed Content (HTTP em HTTPS)."""
        if not url:
            self.send_error(404)
            return

        with LOGO_CACHE_LOCK:
            if url in LOGO_CACHE:
                cached_data, ctype = LOGO_CACHE[url]
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "public, max-age=86400")
                self.send_header("Content-Length", str(len(cached_data)))
                self.end_headers()
                self.wfile.write(cached_data)
                return

        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=4) as resp:
                data = resp.read()
                ctype = resp.headers.get("Content-Type", "image/png")
                with LOGO_CACHE_LOCK:
                    if len(LOGO_CACHE) < 500:
                        LOGO_CACHE[url] = (data, ctype)
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "public, max-age=86400")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        except Exception:
            self.send_error(404)

    def stream_live_ts(self):
        self.send_response(200)
        self.send_header("Content-Type", "video/mp2t")
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        q = HUB.subscribe()
        try:
            while True:
                try:
                    chunk = q.get(timeout=5.0)
                    self.wfile.write(chunk)
                except queue.Empty:
                    continue
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass
        finally:
            HUB.unsubscribe(q)

    def send_status_json(self):
        client_data = None
        if HUB.client_telemetry:
            client_data = dict(HUB.client_telemetry)
            client_data["last_seen_secs"] = round(time.time() - HUB.telemetry_time, 1)

        data = {
            "active_channel_id": HUB.current_channel_id,
            "active_channel_name": HUB.current_channel_name,
            "active_url": HUB.current_url,
            "listeners": len(HUB.subscribers),
            "in_standby": HUB.in_standby,
            "total_mb": round(HUB.total_bytes / (1024 * 1024), 2),
            "uptime_secs": int(time.time() - HUB.start_time),
            "stream_health": HUB.stream_health,
            "stream_bitrate_kbps": round(HUB.get_current_bitrate_kbps(), 1),
            "slate_mode": HUB.slate_mode,
            "slate_active_secs": round(time.time() - HUB.slate_start_time, 1) if HUB.slate_mode else 0,
            "youtube": HUB.youtube_meta,
            "active_vod": ACTIVE_VOD_TASK,
            "vod_display_name": VOD_TASKS[ACTIVE_VOD_TASK].get("display_name") if (ACTIVE_VOD_TASK and ACTIVE_VOD_TASK in VOD_TASKS) else None,
            "is_temporary": HUB.current_is_temporary,
            "client": client_data
        }
        res = json.dumps(data).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(res)

    def send_channels_json(self):
        channels = CH_MGR.get_all()
        res = json.dumps(channels, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "public, max-age=60")
        self.end_headers()
        self.wfile.write(res)

    def send_groups_json(self):
        res = json.dumps(SMART_GROUPS, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(res)

    def send_remote_m3u(self):
        channels = CH_MGR.get_all()
        host = self.headers.get("Host", "127.0.0.1:8080")
        lines = ["#EXTM3U"]
        
        for cid, ch in channels.items():
            name = ch.get("name", cid)
            group = ch.get("category", "General")
            logo = ch.get("logo", "")
            if not logo.startswith("http"):
                logo = ""
            
            line1 = f'#EXTINF:-1 tvg-id="{cid}" tvg-name="{name}" tvg-logo="{logo}" group-title="{group}",{name}'
            line2 = f'http://{host}/remote/{cid}.ts'
            lines.append(line1)
            lines.append(line2)
            
        res = "\n".join(lines).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(res)

    def handle_remote_play(self, path):
        basename = path.split("/")[-1]
        cid = basename.replace(".ts", "")
        
        channels = CH_MGR.get_all()
        if cid not in channels:
            self.send_error(404, "Channel not found")
            return
            
        HUB.switch_channel(cid)
        print(f"[REMOTE] Triggered switch to {cid} via M3U. Serving live stream.")
            
        # Retorna o fluxo real de vídeo em vez da tela preta!
        self.stream_live_ts()

    def proxy_player_api(self, method="GET"):
        parsed = urllib.parse.urlparse(self.path)
        query_str = parsed.query
        now = time.time()

        body = None
        headers = {
            "User-Agent": self.headers.get("User-Agent", "IPTVSmarters/3.1.1"),
            "Accept": "*/*",
        }

        if method == "POST":
            length = int(self.headers.get("Content-Length", 0))
            if length > 0:
                body = self.rfile.read(length)
                headers["Content-Type"] = self.headers.get("Content-Type", "application/x-www-form-urlencoded")

        # Determina action da query ou do body
        query = urllib.parse.parse_qs(query_str)
        action = query.get("action", [""])[0]
        if not action and body:
            try:
                post_data = urllib.parse.parse_qs(body.decode("utf-8", "ignore"))
                action = post_data.get("action", [""])[0]
            except Exception:
                pass

        is_list_call = any(k in action for k in ["get_live", "get_vod", "get_series"]) or any(k in query_str for k in ["get_live", "get_vod", "get_series"])
        cache_key = f"{parsed.path}:{query_str}:{body.decode('utf-8', 'ignore') if body else ''}"

        # Cache para chamadas pesadas de lista (live streams / categories / series / vod) por 600s
        if is_list_call:
            with XTREAM_CACHE_LOCK:
                if cache_key in XTREAM_CACHE:
                    c_data, c_ctype, c_exp = XTREAM_CACHE[cache_key]
                    if now < c_exp:
                        self.send_response(200)
                        self.send_header("Content-Type", c_ctype)
                        self.send_header("Access-Control-Allow-Origin", "*")
                        self.send_header("Content-Length", str(len(c_data)))
                        self.end_headers()
                        self.wfile.write(c_data)
                        return

        upstream_url = f"{XTREAM_UPSTREAM}{parsed.path}"
        if query_str:
            upstream_url += f"?{query_str}"

        data = None
        ctype = "application/json"
        last_err = None

        # Tenta com retry automático e timeout alargado (45s para listas pesadas de 16MB)
        for attempt in range(2):
            try:
                req = urllib.request.Request(upstream_url, data=body, headers=headers, method=method)
                with urllib.request.urlopen(req, timeout=45) as resp:
                    data = resp.read()
                    ctype = resp.headers.get("Content-Type", "application/json")
                last_err = None
                break
            except Exception as e:
                last_err = e
                print(f"[!] Tentativa {attempt + 1} falhou no proxy Xtream ({e})...")
                time.sleep(1)

        if data is None:
            print(f"[!] Erro definitivo no proxy Xtream: {last_err}")
            self.send_error(502, f"Upstream error: {last_err}")
            return

        try:
            # Se for login/autenticação (sem action), reescreve a URL do server_info para a nossa VPS!
            if not action and (b"server_info" in data):
                try:
                    js = json.loads(data.decode("utf-8", "ignore"))
                    if "server_info" in js:
                        host_header = self.headers.get("Host", "tv.smre.run.place")
                        host_name = host_header.split(":")[0]
                        proto = self.headers.get("X-Forwarded-Proto") or ("https" if "443" in host_header else "http")
                        port = "443" if proto == "https" else "80"

                        js["server_info"]["url"] = host_name
                        js["server_info"]["port"] = port
                        js["server_info"]["server_protocol"] = proto
                        js["server_info"]["https_port"] = "443"
                        data = json.dumps(js).encode("utf-8")
                except Exception as ex:
                    print(f"[!] Erro ao reescrever server_info: {ex}")
            elif is_list_call:
                with XTREAM_CACHE_LOCK:
                    if len(XTREAM_CACHE) > 50:
                        XTREAM_CACHE.clear()
                    XTREAM_CACHE[cache_key] = (data, ctype, now + 600.0)

            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as e:
            print(f"[!] Erro ao responder proxy Xtream: {e}")

    def resolve_stream_url(self, url):
        """
        Resolve redirecionamentos HTTP 302/301 e remove porta :80/ explícita
        para evitar bloqueio 403 da Cloudflare/CDN em filmes e séries.
        """
        if not (url.startswith("http://") or url.startswith("https://")):
            return url

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):
                return None

        # Tenta primeiro via proxy residencial (Privoxy -> Chisel SOCKS5)
        for use_proxy in (True, False):
            try:
                handlers = [NoRedirect]
                if use_proxy and RESIDENTIAL_HTTP_PROXY:
                    handlers.append(urllib.request.ProxyHandler({
                        'http': RESIDENTIAL_HTTP_PROXY,
                        'https': RESIDENTIAL_HTTP_PROXY
                    }))
                opener = urllib.request.build_opener(*handlers)
                req = urllib.request.Request(url, headers={"User-Agent": "IPTVSmartersPro"})
                opener.open(req, timeout=5)
                return url
            except urllib.error.HTTPError as e:
                if e.code in (301, 302, 303, 307, 308):
                    loc = e.headers.get("Location")
                    if loc:
                        clean_loc = loc.replace(":80/", "/")
                        print(f"[✓] VOD Redirecionamento resolvido: {clean_loc[:70]}...")
                        return clean_loc
                return url
            except Exception as ex:
                if use_proxy:
                    continue
                print(f"[!] Erro ao resolver redirecionamento VOD: {ex}")
                return url
        return url


    def handle_xtream_stream(self, kind, user, pwd, stream_id, ext):
        if kind == "movie":
            target_url = f"{XTREAM_UPSTREAM}/movie/{user}/{pwd}/{stream_id}.{ext or 'mp4'}"
            cname = f"Filme {stream_id}"
        elif kind == "series":
            target_url = f"{XTREAM_UPSTREAM}/series/{user}/{pwd}/{stream_id}.{ext or 'mp4'}"
            cname = f"Série {stream_id}"
        else:
            # Padrão: canal ao vivo
            target_url = f"{XTREAM_UPSTREAM}/live/{user}/{pwd}/{stream_id}.m3u8"
            cname = f"Canal {stream_id}"

        target_url = self.resolve_stream_url(target_url)

        print(f"\n[⚡ XTREAM] App selecionou: {cname} -> {target_url}")
        HUB.switch_channel(custom_url=target_url, custom_name=cname)

        # Transmite ao vivo para o player do app simultaneamente
        self.stream_live_ts()

    def handle_switch(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8")
        try:
            params = json.loads(body)
        except Exception:
            params = urllib.parse.parse_qs(body)
            params = {k: v[0] for k, v in params.items()}

        # Validação do PIN de Segurança (1233)
        req_pin = self.headers.get("X-Auth-PIN") or params.get("pin")
        if AUTH_PIN and req_pin != AUTH_PIN:
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"success": False, "error": "PIN incorreto"}).encode("utf-8"))
            return

        ch_id = params.get("channel_id")
        custom_url = params.get("url")
        custom_name = params.get("name")

        if custom_url and any(x in custom_url.lower() for x in ["youtube.com", "youtu.be"]):
            try:
                meta = resolve_youtube(custom_url)
                ok = HUB.switch_youtube(meta)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps({
                    "success": ok,
                    "channel": f"YouTube: {meta['title']}",
                    "title": meta["title"],
                    "duration": meta.get("duration"),
                    "thumbnail": meta.get("thumbnail")
                }).encode("utf-8"))
                return
            except Exception as e:
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps({"success": False, "error": f"Erro YouTube: {str(e)}"}).encode("utf-8"))
                return

        ok = HUB.switch_channel(channel_id=ch_id, custom_url=custom_url, custom_name=custom_name)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": ok, "channel": HUB.current_channel_name}).encode("utf-8"))

    def handle_youtube(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8")
        try:
            params = json.loads(body)
        except Exception:
            params = urllib.parse.parse_qs(body)
            params = {k: v[0] for k, v in params.items()}

        # Validação do PIN de Segurança (1233)
        req_pin = self.headers.get("X-Auth-PIN") or params.get("pin")
        if AUTH_PIN and req_pin != AUTH_PIN:
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"success": False, "error": "PIN incorreto"}).encode("utf-8"))
            return

        yt_url = (params.get("url") or "").strip()
        if not yt_url:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"success": False, "error": "URL do YouTube não fornecida"}).encode("utf-8"))
            return

        try:
            meta = resolve_youtube(yt_url)
        except Exception as e:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"success": False, "error": str(e)}).encode("utf-8"))
            return

        ok = HUB.switch_youtube(meta)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({
            "success": ok,
            "channel": f"YouTube: {meta['title']}",
            "title": meta["title"],
            "duration": meta.get("duration"),
            "thumbnail": meta.get("thumbnail")
        }).encode("utf-8"))

    def handle_sync(self):
        """Executa sincronização dos canais e recarga de tarefas VOD em segundo plano."""
        def do_sync():
            try:
                load_vod_tasks()
                import sync_iptv
                sync_iptv.sync()
                CH_MGR.reload()
            except Exception as e:
                print(f"[!] Erro no sync: {e}")
        threading.Thread(target=do_sync, daemon=True).start()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "message": "Sincronização iniciada."}).encode("utf-8"))

    def handle_telemetry(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8")
        try:
            data = json.loads(body)
            HUB.client_telemetry = data
            HUB.telemetry_time = time.time()
            cmd = HUB.pending_command
            HUB.pending_command = None
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"success": True, "cmd": cmd}).encode("utf-8"))
        except Exception as e:
            self.send_response(400)
            self.end_headers()

    def handle_telemetry_result(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8")
        try:
            data = json.loads(body)
            HUB.command_output = data.get("output")
            HUB.command_done_event.set()
            self.send_response(200)
            self.end_headers()
        except Exception:
            self.send_response(400)
            self.end_headers()

    def handle_remote_exec(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8")
        try:
            params = json.loads(body)
        except Exception:
            params = {}
        req_pin = self.headers.get("X-Auth-PIN") or params.get("pin")
        if AUTH_PIN and req_pin != AUTH_PIN:
            self.send_response(401)
            self.end_headers()
            return
        cmd = params.get("cmd")
        if not cmd:
            self.send_response(400)
            self.end_headers()
            return
        HUB.command_done_event.clear()
        HUB.command_output = None
        HUB.pending_command = cmd
        HUB.command_done_event.wait(timeout=10.0)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "output": HUB.command_output or "Timeout aguardando aparelho (está online?)."}).encode("utf-8"))

    def handle_tablet_cmd_res(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", errors="replace") if length > 0 else ""
        HUB.tablet_cmd_res = body
        HUB.tablet_cmd_event.set()
        self.send_response(200)
        self.end_headers()

    def handle_tablet_exec(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        try:
            params = json.loads(body)
        except Exception:
            params = {}
        cmd = params.get("cmd")
        if not cmd:
            self.send_response(400)
            self.end_headers()
            return
        HUB.tablet_cmd_event.clear()
        HUB.tablet_cmd_res = None
        HUB.tablet_pending_cmd = cmd
        HUB.tablet_cmd_event.wait(timeout=10.0)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "output": HUB.tablet_cmd_res or "Timeout aguardando tablet (watchdog ativo?)."}).encode("utf-8"))

    def handle_reset_epoch(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        try:
            params = json.loads(body)
        except Exception:
            params = {}
        req_pin = self.headers.get("X-Auth-PIN") or params.get("pin")
        if AUTH_PIN and req_pin != AUTH_PIN:
            self.send_response(401)
            self.end_headers()
            return
        HUB.reset_pts_epoch()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "message": "PTS epoch reset to 3.0s"}).encode("utf-8"))

    def send_fuse_bin(self, fname="fuse_direct_arm_verified"):
        fpath = os.path.join(CONFIG_DIR, fname)
        if not os.path.exists(fpath):
            self.send_error(404, "Binary not found")
            return
        with open(fpath, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_vod_prepare(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        try:
            params = json.loads(body)
        except Exception:
            params = urllib.parse.parse_qs(body)
            params = {k: v[0] for k, v in params.items()}

        req_pin = self.headers.get("X-Auth-PIN") or params.get("pin")
        if AUTH_PIN and req_pin != AUTH_PIN:
            self.send_response(401)
            self.end_headers()
            return

        url = (params.get("url") or "").strip()
        title = (params.get("title") or "").strip()
        if not url:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"success": False, "error": "URL não fornecida"}).encode("utf-8"))
            return

        task_id = hashlib.md5(url.encode("utf-8")).hexdigest()[:8]
        with VOD_TASKS_LOCK:
            existing = VOD_TASKS.get(task_id)
            if existing and existing.get("status") == "ready" and os.path.exists(existing.get("file_path", "")):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps({
                    "success": True,
                    "task_id": task_id,
                    "status": "ready",
                    "display_name": existing.get("display_name", "")
                }).encode("utf-8"))
                return

            VOD_TASKS[task_id] = {
                "id": task_id,
                "url": url,
                "title": title or "Vídeo VOD",
                "display_name": "",
                "status": "pending",
                "progress": 0,
                "error": None,
                "created_at": time.time()
            }
            save_vod_tasks()

        threading.Thread(target=_prepare_vod_thread, args=(task_id, url, title), daemon=True).start()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "task_id": task_id, "status": "pending"}).encode("utf-8"))

    def send_vod_status(self):
        global ACTIVE_VOD_TASK
        with VOD_TASKS_LOCK:
            tasks_list = list(VOD_TASKS.values())
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({
            "active_vod": ACTIVE_VOD_TASK,
            "tasks": tasks_list
        }).encode("utf-8"))

    def handle_vod_play(self):
        global ACTIVE_VOD_TASK
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        try:
            params = json.loads(body)
        except Exception:
            params = urllib.parse.parse_qs(body)
            params = {k: v[0] for k, v in params.items()}

        req_pin = self.headers.get("X-Auth-PIN") or params.get("pin")
        if AUTH_PIN and req_pin != AUTH_PIN:
            self.send_response(401)
            self.end_headers()
            return

        task_id = params.get("task_id")
        with VOD_TASKS_LOCK:
            task = VOD_TASKS.get(task_id)
        if not task or task.get("status") != "ready":
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"success": False, "error": "VOD não está pronto"}).encode("utf-8"))
            return

        ACTIVE_VOD_TASK = task_id
        dispatch_device_cmd(f"sh /data/local/tmp/switch_vod.sh {task_id}")
        log_event(f"VOD_PLAY {task_id} ({task.get('display_name')})")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "task_id": task_id, "display_name": task.get("display_name")}).encode("utf-8"))

    def handle_vod_live(self):
        global ACTIVE_VOD_TASK
        ACTIVE_VOD_TASK = None
        dispatch_device_cmd("sh /data/local/tmp/switch_live.sh")
        log_event("VOD_RETURN_LIVE")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "status": "live"}).encode("utf-8"))

    def handle_vod_stream(self, path):
        parts = [p for p in path.strip("/").split("/") if p]
        if len(parts) < 3:
            self.send_error(404, "Invalid VOD path")
            return
        task_id = parts[1]
        filename = parts[2]

        with VOD_TASKS_LOCK:
            task = VOD_TASKS.get(task_id)

        if not task:
            f_mp4 = os.path.join(VOD_DIR, f"{task_id}.mp4")
            f_bin = os.path.join(VOD_DIR, f"{task_id}.bin")
            if os.path.exists(f_mp4):
                task = {
                    "file_path": f_mp4,
                    "template_path": f_bin,
                    "status": "ready"
                }
            else:
                self.send_error(404, "VOD task not found")
                return

        if filename == "template.bin":
            tmpl_path = task.get("template_path")
            if not tmpl_path or not os.path.exists(tmpl_path):
                self.send_error(404, "Template not found")
                return
            with open(tmpl_path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
            return

        if filename in ("movie.mp4", "movie.ts"):
            file_path = task.get("file_path")
            if not file_path or not os.path.exists(file_path):
                self.send_error(404, "Movie file not found")
                return
            file_size = os.path.getsize(file_path)
            range_header = self.headers.get("Range")

            if range_header:
                m = re.match(r"bytes=(\d+)-(\d*)", range_header)
                if m:
                    start = int(m.group(1))
                    end = int(m.group(2)) if m.group(2) else file_size - 1
                    if start >= file_size:
                        self.send_response(416, "Range Not Satisfiable")
                        self.send_header("Content-Range", f"bytes */{file_size}")
                        self.end_headers()
                        return
                    if end >= file_size:
                        end = file_size - 1
                    content_len = end - start + 1
                    self.send_response(206, "Partial Content")
                    self.send_header("Content-Type", "video/mp4")
                    self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
                    self.send_header("Content-Length", str(content_len))
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    with open(file_path, "rb") as f:
                        f.seek(start)
                        rem = content_len
                        while rem > 0:
                            chunk = f.read(min(131072, rem))
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                            rem -= len(chunk)
                    return

            self.send_response(200)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Content-Length", str(file_size))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with open(file_path, "rb") as f:
                while True:
                    chunk = f.read(131072)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return

        self.send_error(404, "Not Found")

    def send_dashboard(self):
        tunnel_url = "http://localhost:8080"
        tunnel_file = os.path.join(CONFIG_DIR, "tunnel_url.txt")
        if os.path.exists(tunnel_file):
            try:
                with open(tunnel_file, "r") as f:
                    tunnel_url = f.read().strip()
            except Exception:
                pass

        html = f"""<!DOCTYPE html>
<html lang="pt-BR">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Controle Remoto - USB Stream TV</title>
    <style>
        :root {{
            --bg: #0a0e17;
            --surface: #131b2e;
            --surface-hover: #1c2742;
            --border: #223152;
            --primary: #38bdf8;
            --primary-glow: rgba(56, 189, 248, 0.25);
            --green: #10b981;
            --red: #ef4444;
            --yellow: #f59e0b;
            --text: #f8fafc;
            --text-dim: #94a3b8;
            --card-radius: 14px;
        }}
        * {{ box-sizing: border-box; -webkit-tap-highlight-color: transparent; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--bg);
            color: var(--text);
            margin: 0;
            padding: 0 0 100px 0;
            display: flex;
            justify-content: center;
        }}
        .app {{
            width: 100%;
            max-width: 720px;
            padding: 12px 16px;
        }}
        /* Header */
        header {{
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 10px 0 16px 0;
            border-bottom: 1px solid var(--border);
        }}
        .logo {{
            font-size: 19px;
            font-weight: 800;
            color: var(--primary);
            display: flex;
            align-items: center;
            gap: 8px;
            letter-spacing: -0.5px;
        }}
        .status-pill {{
            font-size: 12px;
            font-weight: 600;
            padding: 4px 10px;
            border-radius: 20px;
            background: rgba(16, 185, 129, 0.15);
            color: var(--green);
            border: 1px solid rgba(16, 185, 129, 0.3);
            display: flex;
            align-items: center;
            gap: 6px;
        }}
        .status-pill.waiting {{
            background: rgba(245, 158, 11, 0.15);
            color: var(--yellow);
            border-color: rgba(245, 158, 11, 0.3);
        }}
        .status-pill.standby {{
            background: rgba(56, 189, 248, 0.15);
            color: var(--primary);
            border-color: rgba(56, 189, 248, 0.4);
        }}
        /* Now Playing Banner */
        .now-card {{
            background: linear-gradient(135deg, #162238 0%, #0c1424 100%);
            border: 2px solid var(--primary);
            border-radius: var(--card-radius);
            padding: 16px;
            margin: 16px 0;
            box-shadow: 0 8px 30px var(--primary-glow);
            display: flex;
            justify-content: space-between;
            align-items: center;
        }}
        .now-label {{
            font-size: 11px;
            font-weight: 700;
            color: var(--text-dim);
            text-transform: uppercase;
            letter-spacing: 0.8px;
            margin-bottom: 6px;
        }}
        .now-title {{
            font-size: 18px;
            font-weight: 700;
            color: #fff;
            display: flex;
            align-items: center;
            gap: 8px;
            max-width: 480px;
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
        }}
        .pulse-dot {{
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background-color: var(--red);
            box-shadow: 0 0 10px var(--red);
            animation: pulse 1.5s infinite;
            flex-shrink: 0;
        }}
        @keyframes pulse {{
            0%, 100% {{ opacity: 1; transform: scale(1); }}
            50% {{ opacity: 0.4; transform: scale(0.85); }}
        }}
        .now-meta {{
            font-size: 12px;
            color: var(--text-dim);
            text-align: right;
            flex-shrink: 0;
        }}
        /* Search Box */
        .search-container {{
            position: sticky;
            top: 0;
            background: var(--bg);
            padding: 10px 0;
            z-index: 30;
        }}
        .search-box {{
            display: flex;
            align-items: center;
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 12px;
            padding: 2px 14px;
            transition: border-color 0.2s;
        }}
        .search-box:focus-within {{
            border-color: var(--primary);
            box-shadow: 0 0 12px var(--primary-glow);
        }}
        .search-box input {{
            flex: 1;
            background: transparent;
            border: none;
            padding: 12px 6px;
            font-size: 15px;
            color: #fff;
            outline: none;
        }}
        .search-box .clear-btn {{
            cursor: pointer;
            color: var(--text-dim);
            font-size: 18px;
            display: none;
            padding: 4px;
        }}
        /* Category Chips Bar */
        .categories-scroll {{
            display: flex;
            gap: 8px;
            overflow-x: auto;
            padding: 6px 0 14px 0;
            scrollbar-width: none;
            -webkit-overflow-scrolling: touch;
        }}
        .categories-scroll::-webkit-scrollbar {{ display: none; }}
        .cat-chip {{
            white-space: nowrap;
            background: var(--surface);
            border: 1px solid var(--border);
            color: var(--text-dim);
            padding: 8px 14px;
            border-radius: 20px;
            font-size: 13px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.2s;
            user-select: none;
        }}
        .cat-chip.active {{
            background: var(--primary);
            color: #080d1a;
            border-color: var(--primary);
            font-weight: 700;
        }}
        /* Grid */
        .channels-info {{
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 12px;
            color: var(--text-dim);
            margin: 6px 0 12px 0;
        }}
        .channels-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(150px, 1fr));
            gap: 10px;
        }}
        .channel-card {{
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: var(--card-radius);
            padding: 12px 10px;
            display: flex;
            flex-direction: column;
            align-items: center;
            text-align: center;
            cursor: pointer;
            transition: transform 0.15s, border-color 0.15s;
            position: relative;
            user-select: none;
        }}
        .channel-card:active {{
            transform: scale(0.97);
        }}
        .channel-card.active {{
            border-color: var(--green);
            background: rgba(16, 185, 129, 0.1);
        }}
        .fav-star {{
            position: absolute;
            top: 8px;
            right: 8px;
            font-size: 16px;
            color: #4b5563;
            cursor: pointer;
            transition: color 0.15s, transform 0.15s;
            padding: 4px;
        }}
        .fav-star.active {{
            color: #facc15;
            text-shadow: 0 0 8px rgba(250, 204, 21, 0.5);
        }}
        .fav-star:active {{
            transform: scale(1.3);
        }}
        .channel-logo-wrap {{
            width: 54px;
            height: 54px;
            display: flex;
            align-items: center;
            justify-content: center;
            margin-bottom: 8px;
            border-radius: 10px;
            background: rgba(0,0,0,0.25);
            overflow: hidden;
        }}
        .channel-logo-img {{
            max-width: 100%;
            max-height: 100%;
            object-fit: contain;
        }}
        .channel-logo-fallback {{
            font-size: 24px;
        }}
        .channel-card-name {{
            font-size: 13px;
            font-weight: 600;
            color: #fff;
            margin-bottom: 6px;
            display: -webkit-box;
            -webkit-line-clamp: 2;
            -webkit-box-orient: vertical;
            overflow: hidden;
            line-height: 1.25;
            min-height: 32px;
        }}
        .channel-badges {{
            display: flex;
            gap: 4px;
            align-items: center;
        }}
        .badge {{
            font-size: 10px;
            font-weight: 700;
            padding: 2px 6px;
            border-radius: 6px;
            background: rgba(255,255,255,0.06);
            color: var(--text-dim);
        }}
        .badge.fhd {{ color: #38bdf8; background: rgba(56, 189, 248, 0.15); }}
        .badge.hd {{ color: #34d399; background: rgba(52, 211, 153, 0.15); }}
        /* Load More */
        .load-more-btn {{
            width: 100%;
            background: var(--surface);
            border: 1px solid var(--border);
            color: var(--text);
            padding: 14px;
            border-radius: 12px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            margin-top: 16px;
            transition: background 0.2s;
        }}
        .load-more-btn:active {{
            background: var(--surface-hover);
        }}
        /* Custom Accordion */
        details.custom-accordion {{
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: var(--card-radius);
            margin-top: 24px;
            overflow: hidden;
        }}
        details.custom-accordion summary {{
            padding: 14px 16px;
            font-size: 14px;
            font-weight: 600;
            color: var(--text-dim);
            cursor: pointer;
            user-select: none;
        }}
        .custom-body {{
            padding: 0 16px 16px 16px;
        }}
        .custom-input-group {{
            display: flex;
            gap: 8px;
            margin-top: 8px;
        }}
        .custom-input-group input {{
            flex: 1;
            background: var(--bg);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 10px 12px;
            color: #fff;
            font-size: 13px;
            outline: none;
        }}
        .btn-play-custom {{
            background: var(--primary);
            color: #0b0f19;
            border: none;
            border-radius: 8px;
            padding: 10px 16px;
            font-weight: 700;
            font-size: 13px;
            cursor: pointer;
        }}
        /* YouTube Card */
        .youtube-card {{
            background: linear-gradient(135deg, #18141e 0%, #0d111d 100%);
            border: 1px solid rgba(239, 68, 68, 0.35);
            border-radius: var(--card-radius);
            padding: 14px 16px;
            margin: 14px 0;
            box-shadow: 0 4px 20px rgba(239, 68, 68, 0.12);
        }}
        .youtube-header {{
            display: flex;
            align-items: center;
            gap: 8px;
        }}
        .btn-play-yt {{
            background: #e50914;
            color: #fff;
            border: none;
            border-radius: 8px;
            padding: 10px 16px;
            font-weight: 700;
            font-size: 13px;
            cursor: pointer;
            display: inline-flex;
            align-items: center;
            gap: 6px;
            white-space: nowrap;
        }}
        .btn-play-yt:active {{
            background: #b80710;
        }}
        .spinner {{
            display: inline-block;
            width: 13px;
            height: 13px;
            border: 2px solid rgba(255,255,255,0.3);
            border-radius: 50%;
            border-top-color: #fff;
            animation: spin 0.8s linear infinite;
        }}
        @keyframes spin {{
            to {{ transform: rotate(360deg); }}
        }}
        /* Toast */
        .toast {{
            position: fixed;
            bottom: 24px;
            left: 50%;
            transform: translateX(-50%);
            background: var(--primary);
            color: #090e17;
            padding: 12px 24px;
            border-radius: 25px;
            font-weight: 700;
            font-size: 14px;
            box-shadow: 0 6px 20px rgba(0,0,0,0.5);
            display: none;
            z-index: 1000;
            text-align: center;
            max-width: 90%;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }}
    </style>
</head>
<body>
    <div class="app">
        <header>
            <div class="logo">⚡ USB Stream TV</div>
            <div id="tv-status" class="status-pill waiting">
                <span>●</span> <span id="tv-status-text">Aguardando TV</span>
            </div>
        </header>

        <!-- Banner Tocando Agora -->
        <div class="now-card">
            <div style="min-width: 0;">
                <div class="now-label">Transmitindo Agora na TV</div>
                <div class="now-title">
                    <span class="pulse-dot"></span>
                    <span id="active-title">Carregando canal...</span>
                </div>
            </div>
            <div class="now-meta">
                <div id="traffic-text" style="font-weight: 700; color: #fff;">0 MB</div>
                <div style="font-size: 10px;">Enviado VPS</div>
                <div id="client-lead-badge" style="font-size: 10px; color: #38bdf8; margin-top: 4px; display: none;"></div>
            </div>
        </div>

        <!-- YouTube Cast & Cinema VOD Card -->
        <div class="youtube-card">
            <div class="youtube-header">
                <span style="font-size: 18px;">🎬</span>
                <span style="font-weight: 700; color: #ef4444; font-size: 14px;">Cinema VOD & YouTube na TV</span>
            </div>
            <div style="font-size: 12px; color: var(--text-dim); margin-top: 4px; margin-bottom: 8px;">
                Filmes, Séries ou Vídeos com <strong>Pause, Seek (Avanço/Volta) e Retomada</strong> nativos no controle da TV.
            </div>
            <div class="custom-input-group" style="flex-wrap: wrap;">
                <input type="text" id="yt-url-input" placeholder="Cole link do YouTube ou Filme (ex: https://youtu.be/...)" onkeydown="if(event.key==='Enter') prepareVOD()">
                <button class="btn-play-yt" id="btn-vod-prep" onclick="prepareVOD()" style="background: #e50914;" title="Prepara o filme com suporte nativo a Pause e Seek na TV">
                    <span id="btn-vod-prep-text">🎬 Preparar Cinema (Seek/Pause)</span>
                    <span id="btn-vod-prep-spin" class="spinner" style="display: none;"></span>
                </button>
                <button class="btn-play-yt" id="btn-yt" onclick="castYouTube()" style="background: #334155;" title="Transmitir diretamente como Live contínua">
                    <span id="btn-yt-text">▶️ Ao Vivo</span>
                    <span id="btn-yt-spin" class="spinner" style="display: none;"></span>
                </button>
            </div>
            
            <!-- VOD Status & Control Box -->
            <div id="vod-status-box" style="display: none; margin-top: 10px; padding: 12px; background: rgba(229, 9, 20, 0.08); border: 1px solid rgba(229, 9, 20, 0.3); border-radius: 8px;">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px;">
                    <span id="vod-movie-title" style="font-weight: 700; font-size: 13px; color: #fff;"></span>
                    <span id="vod-status-badge" style="font-size: 11px; padding: 2px 8px; border-radius: 4px; background: #2563eb; color: #fff;">Preparando</span>
                </div>
                <div id="vod-progress-bar-bg" style="width: 100%; height: 6px; background: rgba(255,255,255,0.1); border-radius: 3px; overflow: hidden; margin-bottom: 8px;">
                    <div id="vod-progress-bar" style="width: 0%; height: 100%; background: #e50914; transition: width 0.3s;"></div>
                </div>
                <div style="display: flex; gap: 8px; align-items: center; flex-wrap: wrap;">
                    <button id="btn-vod-play" onclick="playVOD()" style="display: none; background: #16a34a; color: #fff; border: none; border-radius: 6px; padding: 8px 14px; font-weight: 700; font-size: 12px; cursor: pointer;">
                        ▶️ Assistir na TV (ConnectShare)
                    </button>
                    <button id="btn-vod-live" onclick="returnLive()" style="display: none; background: #475569; color: #fff; border: none; border-radius: 6px; padding: 8px 14px; font-weight: 700; font-size: 12px; cursor: pointer;">
                        📺 Voltar para TV Ao Vivo
                    </button>
                    <span id="vod-hint" style="font-size: 11px; color: var(--text-dim);"></span>
                </div>
            </div>

            <div id="yt-active-box" style="display: none; margin-top: 10px; padding: 10px; background: rgba(239, 68, 68, 0.08); border: 1px solid rgba(239, 68, 68, 0.25); border-radius: 8px;">
                <div style="display: flex; gap: 10px; align-items: center;">
                    <img id="yt-thumb" style="width: 72px; height: 42px; border-radius: 4px; object-fit: cover; background: #000;" src="" />
                    <div style="flex: 1; min-width: 0;">
                        <div id="yt-title" style="font-size: 12px; font-weight: 700; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: #fff;"></div>
                        <div id="yt-meta-info" style="font-size: 11px; color: var(--text-dim); margin-top: 2px;"></div>
                    </div>
                </div>
            </div>
        </div>

        <!-- Barra de Busca Fixa -->
        <div class="search-container">
            <div class="search-box">
                <span style="font-size: 16px; margin-right: 6px;">🔍</span>
                <input type="text" id="search-input" placeholder="Buscar canal (ex: Globo, SporTV, ESPN, Gazeta, Filmes)..." oninput="onSearchInput()">
                <span class="clear-btn" id="clear-btn" onclick="clearSearch()">×</span>
            </div>
        </div>

        <!-- Carrossel de Categorias -->
        <div class="categories-scroll" id="categories-bar">
            <!-- Categorias injetadas dinamicamente -->
        </div>

        <!-- Grade de Canais -->
        <div class="channels-info">
            <span id="match-counter">Carregando canais...</span>
            <span style="cursor: pointer; color: var(--primary);" onclick="syncChannels()">🔄 Atualizar Grade</span>
        </div>

        <div class="channels-grid" id="channels-grid"></div>

        <button id="load-more-btn" class="load-more-btn" style="display: none;" onclick="loadMore()">Carregar mais canais...</button>

        <!-- URL Personalizada -->
        <details class="custom-accordion">
            <summary>🔗 Transmitir Link Manual / Outro IPTV</summary>
            <div class="custom-body">
                <div class="custom-input-group">
                    <input type="text" id="custom-url-input" placeholder="https://.../stream.m3u8 ou canal.ts">
                    <button class="btn-play-custom" onclick="playCustom()">Tocar</button>
                </div>
            </div>
        </details>

        <div id="toast" class="toast">Canal alterado com sucesso!</div>
    </div>

    <script>
        let allChannels = [];
        let smartGroups = [];
        let filteredChannels = [];
        let currentGroupId = 'all';
        let currentSearch = '';
        let currentActiveId = '';
        let favorites = new Set();
        let displayLimit = 48;

        // Carrega favoritos do LocalStorage (iniciando com canais populares verificados)
        try {{
            const savedFavs = localStorage.getItem('tv_favs_v3');
            if (savedFavs) {{
                favorites = new Set(JSON.parse(savedFavs));
            }} else {{
                favorites = new Set(['band-rio', 'globo-rj', 'record-news', 'tv-cultura-sp', 'studio-universal-br', 'sony-channel-br', 'espn-mirror-a07z', 'espn4-mirror-a07n']);
                localStorage.setItem('tv_favs_v3', JSON.stringify(Array.from(favorites)));
            }}
        }} catch(e) {{}}

        function saveFavorites() {{
            try {{
                localStorage.setItem('tv_favs_v3', JSON.stringify(Array.from(favorites)));
            }} catch(e) {{}}
        }}

        function toggleFavorite(id, e) {{
            e.stopPropagation();
            if (favorites.has(id)) {{
                favorites.delete(id);
                showToast("Removido dos favoritos.");
            }} else {{
                favorites.add(id);
                showToast("Adicionado aos favoritos!");
            }}
            saveFavorites();
            if (currentGroupId === 'fav') {{
                applyFilters();
            }} else {{
                const star = document.getElementById(`star-${{id}}`);
                if (star) star.classList.toggle('active', favorites.has(id));
            }}
        }}

        async function init() {{
            try {{
                const [grpRes, chRes] = await Promise.all([
                    fetch('/api/groups'),
                    fetch('/api/channels')
                ]);
                smartGroups = await grpRes.json();
                const channelsObj = await chRes.json();
                allChannels = Object.values(channelsObj);
                renderCategories();
                applyFilters();
                updateStatus();
                setInterval(updateStatus, 2000);
                pollVODStatus();
                setInterval(pollVODStatus, 2500);
            }} catch (e) {{
                console.error(e);
            }}
        }}

        function renderCategories() {{
            const bar = document.getElementById('categories-bar');
            bar.innerHTML = '';
            smartGroups.forEach(g => {{
                const chip = document.createElement('div');
                chip.className = `cat-chip ${{g.id === currentGroupId ? 'active' : ''}}`;
                chip.id = `chip-${{g.id}}`;
                chip.innerHTML = `${{g.icon}} ${{g.name}}`;
                chip.onclick = () => selectCategory(g.id);
                bar.appendChild(chip);
            }});
        }}

        function selectCategory(gid) {{
            currentGroupId = gid;
            document.querySelectorAll('.cat-chip').forEach(c => c.classList.remove('active'));
            const activeChip = document.getElementById(`chip-${{gid}}`);
            if (activeChip) activeChip.classList.add('active');
            displayLimit = 48;
            applyFilters();
        }}

        function onSearchInput() {{
            const val = document.getElementById('search-input').value;
            currentSearch = val.trim().toLowerCase();
            document.getElementById('clear-btn').style.display = currentSearch ? 'block' : 'none';
            displayLimit = 48;
            applyFilters();
        }}

        function clearSearch() {{
            document.getElementById('search-input').value = '';
            currentSearch = '';
            document.getElementById('clear-btn').style.display = 'none';
            applyFilters();
        }}

        function applyFilters() {{
            const group = smartGroups.find(g => g.id === currentGroupId);
            filteredChannels = allChannels.filter(ch => {{
                // 1. Filtro de Grupo
                if (group) {{
                    if (group.id === 'fav') {{
                        if (!favorites.has(ch.id)) return false;
                    }} else if (group.id !== 'all') {{
                        if (!group.categories.includes(ch.category)) return false;
                    }}
                }}
                // 2. Filtro de Busca
                if (currentSearch) {{
                    const matchName = ch.name.toLowerCase().includes(currentSearch);
                    const matchCat = (ch.category || '').toLowerCase().includes(currentSearch);
                    if (!matchName && !matchCat) return false;
                }}
                return true;
            }});

            document.getElementById('match-counter').innerText =
                `${{filteredChannels.length}} canais encontrados`;

            renderGrid();
        }}

        function renderGrid() {{
            const grid = document.getElementById('channels-grid');
            grid.innerHTML = '';
            const slice = filteredChannels.slice(0, displayLimit);

            slice.forEach(ch => {{
                const isFav = favorites.has(ch.id);
                const isActive = ch.id === currentActiveId;
                const card = document.createElement('div');
                card.className = `channel-card ${{isActive ? 'active' : ''}}`;
                card.id = `card-${{ch.id}}`;
                card.onclick = () => switchChannel(ch.id);

                let logoHtml = '';
                if (ch.logo && ch.logo.startsWith('http')) {{
                    const proxied = `/api/logo?url=${{encodeURIComponent(ch.logo)}}`;
                    logoHtml = `<img class="channel-logo-img" src="${{proxied}}" loading="lazy" onerror="this.parentElement.innerHTML='📺'">`;
                }} else {{
                    logoHtml = `<span class="channel-logo-fallback">${{ch.logo || '📺'}}</span>`;
                }}

                card.innerHTML = `
                    <span class="fav-star ${{isFav ? 'active' : ''}}" id="star-${{ch.id}}" onclick="toggleFavorite('${{ch.id}}', event)">★</span>
                    <div class="channel-logo-wrap">${{logoHtml}}</div>
                    <div class="channel-card-name" title="${{ch.name}}">${{ch.name}}</div>
                    <div class="channel-badges">
                        <span class="badge ${{ch.quality ? ch.quality.toLowerCase() : ''}}">${{ch.quality || 'HD'}}</span>
                        <span class="badge">${{ch.category || 'Geral'}}</span>
                    </div>
                `;
                grid.appendChild(card);
            }});

            const loadMoreBtn = document.getElementById('load-more-btn');
            if (filteredChannels.length > displayLimit) {{
                loadMoreBtn.style.display = 'block';
                loadMoreBtn.innerText = `Carregar mais (${{filteredChannels.length - displayLimit}} restantes)...`;
            }} else {{
                loadMoreBtn.style.display = 'none';
            }}
        }}

        function loadMore() {{
            displayLimit += 48;
            renderGrid();
        }}

        function getAuthPin() {{
            let pin = localStorage.getItem('tv_pin');
            if (!pin) {{
                pin = prompt("Digite o PIN de acesso:");
                if (pin) {{
                    localStorage.setItem('tv_pin', pin.trim());
                }}
            }}
            return pin || '';
        }}

        async function switchChannel(id) {{
            const pin = getAuthPin();
            if (!pin) {{
                showToast("PIN necessário para trocar canal.");
                return;
            }}
            showToast("Sintonizando canal na TV...");
            try {{
                const res = await fetch('/api/switch', {{
                    method: 'POST',
                    headers: {{ 
                        'Content-Type': 'application/json',
                        'X-Auth-PIN': pin
                    }},
                    body: JSON.stringify({{ channel_id: id, pin: pin }})
                }});
                if (res.status === 401) {{
                    localStorage.removeItem('tv_pin');
                    showToast("PIN incorreto. Tente novamente.");
                    return;
                }}
                const data = await res.json();
                if (data.success) {{
                    currentActiveId = id;
                    document.getElementById('active-title').innerText = data.channel;
                    showToast(`Sintonizado: ${{data.channel}}`);
                    document.querySelectorAll('.channel-card').forEach(c => c.classList.remove('active'));
                    const curCard = document.getElementById(`card-${{id}}`);
                    if (curCard) curCard.classList.add('active');
                }}
            }} catch (e) {{
                showToast("Erro ao trocar canal.");
            }}
        }}

        async function playCustom() {{
            const pin = getAuthPin();
            if (!pin) return;
            const url = document.getElementById('custom-url-input').value.trim();
            if (!url) return;
            showToast("Conectando stream manual...");
            try {{
                const res = await fetch('/api/switch', {{
                    method: 'POST',
                    headers: {{ 
                        'Content-Type': 'application/json',
                        'X-Auth-PIN': pin
                    }},
                    body: JSON.stringify({{ url: url, name: "Stream Manual", pin: pin }})
                }});
                if (res.status === 401) {{
                    localStorage.removeItem('tv_pin');
                    showToast("PIN incorreto.");
                    return;
                }}
                const data = await res.json();
                if (data.success) {{
                    showToast("Transmitindo stream manual!");
                    updateStatus();
                }}
            }} catch (e) {{
                showToast("Erro ao conectar.");
            }}
        }}

        async function castYouTube() {{
            const pin = getAuthPin();
            if (!pin) return;
            const input = document.getElementById('yt-url-input');
            const url = input.value.trim();
            if (!url) {{
                showToast("Cole o link de um vídeo do YouTube.");
                return;
            }}
            const btn = document.getElementById('btn-yt');
            const btnText = document.getElementById('btn-yt-text');
            const btnSpin = document.getElementById('btn-yt-spin');
            btn.disabled = true;
            btnText.innerText = "Processando...";
            btnSpin.style.display = "inline-block";
            showToast("Extraindo vídeo do YouTube...");

            try {{
                const res = await fetch('/api/youtube', {{
                    method: 'POST',
                    headers: {{ 
                        'Content-Type': 'application/json',
                        'X-Auth-PIN': pin
                    }},
                    body: JSON.stringify({{ url: url, pin: pin }})
                }});
                if (res.status === 401) {{
                    localStorage.removeItem('tv_pin');
                    showToast("PIN incorreto.");
                    return;
                }}
                const data = await res.json();
                if (data.success) {{
                    showToast(`Transmitindo na TV: ${{data.title}}`);
                    input.value = "";
                    updateStatus();
                }} else {{
                    showToast(data.error || "Erro ao transmitir vídeo.");
                }}
            }} catch (e) {{
                showToast("Erro ao conectar com o servidor.");
            }} finally {{
                btn.disabled = false;
                btnText.innerText = "Transmitir";
                btnSpin.style.display = "none";
            }}
        }}

        let activeVodTaskId = null;

        async function prepareVOD() {{
            const input = document.getElementById('yt-url-input');
            const url = (input.value || '').trim();
            if (!url) {{
                showToast("Por favor, cole um link de vídeo ou filme.");
                return;
            }}

            const pin = getAuthPin();
            const btn = document.getElementById('btn-vod-prep');
            const btnText = document.getElementById('btn-vod-prep-text');
            const btnSpin = document.getElementById('btn-vod-prep-spin');

            btn.disabled = true;
            btnText.innerText = "Preparando...";
            btnSpin.style.display = "inline-block";

            try {{
                const res = await fetch('/api/vod/prepare', {{
                    method: 'POST',
                    headers: {{
                        'Content-Type': 'application/json',
                        'X-Auth-PIN': pin
                    }},
                    body: JSON.stringify({{ url: url, pin: pin }})
                }});

                if (res.status === 401) {{
                    localStorage.removeItem('tv_pin');
                    showToast("PIN incorreto.");
                    return;
                }}

                const data = await res.json();
                if (data.success && data.task_id) {{
                    activeVodTaskId = data.task_id;
                    showToast("Preparando vídeo com áudio AC3 para a TV...");
                    input.value = "";
                    pollVODStatus();
                }} else {{
                    showToast(data.error || "Erro ao iniciar preparação do filme.");
                }}
            }} catch (e) {{
                showToast("Erro ao conectar com o servidor.");
            }} finally {{
                btn.disabled = false;
                btnText.innerText = "🎬 Preparar Cinema (Seek/Pause)";
                btnSpin.style.display = "none";
            }}
        }}

        async function pollVODStatus() {{
            try {{
                const res = await fetch('/api/vod/status');
                const data = await res.json();
                const box = document.getElementById('vod-status-box');
                const titleEl = document.getElementById('vod-movie-title');
                const badgeEl = document.getElementById('vod-status-badge');
                const barEl = document.getElementById('vod-progress-bar');
                const btnPlay = document.getElementById('btn-vod-play');
                const btnLive = document.getElementById('btn-vod-live');
                const hintEl = document.getElementById('vod-hint');

                if (!data.tasks || data.tasks.length === 0) {{
                    box.style.display = 'none';
                    return;
                }}

                let task = null;
                if (data.active_vod) {{
                    task = data.tasks.find(t => t.id === data.active_vod);
                }}
                if (!task && activeVodTaskId) {{
                    task = data.tasks.find(t => t.id === activeVodTaskId);
                }}
                if (!task) {{
                    task = data.tasks[data.tasks.length - 1];
                }}

                if (!task) {{
                    box.style.display = 'none';
                    return;
                }}

                activeVodTaskId = task.id;
                box.style.display = 'block';
                titleEl.innerText = task.display_name || task.id;

                if (task.status === 'processing') {{
                    const pct = task.progress || 5;
                    badgeEl.innerText = `${{pct}}%`;
                    badgeEl.style.background = '#2563eb';
                    barEl.style.width = `${{pct}}%`;
                    barEl.style.background = '#e50914';
                    btnPlay.style.display = 'none';
                    btnLive.style.display = 'none';
                    hintEl.innerText = 'Convertendo e indexando com áudio AC3 Samsung...';
                }} else if (task.status === 'ready') {{
                    barEl.style.width = '100%';
                    if (data.active_vod === task.id) {{
                        badgeEl.innerText = 'Transmitindo no ConnectShare';
                        badgeEl.style.background = '#e50914';
                        barEl.style.background = '#e50914';
                        btnPlay.style.display = 'none';
                        btnLive.style.display = 'inline-block';
                        hintEl.innerText = `Tocando na TV: "${{task.display_name}}". Use Pause/Seek à vontade!`;
                    }} else {{
                        badgeEl.innerText = 'Pronto para TV';
                        badgeEl.style.background = '#16a34a';
                        barEl.style.background = '#16a34a';
                        btnPlay.style.display = 'inline-block';
                        btnPlay.innerText = `▶️ Assistir "${{task.display_name}}" na TV`;
                        btnLive.style.display = 'none';
                        hintEl.innerText = 'Clique para enviar à TV com suporte total a Pause e Seek.';
                    }}
                }} else if (task.status === 'error') {{
                    badgeEl.innerText = 'Erro';
                    badgeEl.style.background = '#dc2626';
                    barEl.style.width = '100%';
                    barEl.style.background = '#dc2626';
                    btnPlay.style.display = 'none';
                    btnLive.style.display = 'none';
                    hintEl.innerText = task.error || 'Falha ao processar arquivo.';
                }}
            }} catch (e) {{}}
        }}

        async function playVOD() {{
            if (!activeVodTaskId) return;
            const pin = getAuthPin();
            const btnPlay = document.getElementById('btn-vod-play');
            btnPlay.disabled = true;
            btnPlay.innerText = "Alternando TV...";

            try {{
                const res = await fetch('/api/vod/play', {{
                    method: 'POST',
                    headers: {{
                        'Content-Type': 'application/json',
                        'X-Auth-PIN': pin
                    }},
                    body: JSON.stringify({{ task_id: activeVodTaskId, pin: pin }})
                }});
                const data = await res.json();
                if (data.success) {{
                    showToast(`TV atualizada! Abra o ConnectShare e selecione "${{data.display_name || 'Filme'}}"`);
                    pollVODStatus();
                }} else {{
                    showToast(data.error || "Erro ao iniciar VOD na TV.");
                }}
            }} catch (e) {{
                showToast("Erro de comunicação com o servidor.");
            }} finally {{
                btnPlay.disabled = false;
            }}
        }}

        async function returnLive() {{
            const pin = getAuthPin();
            const btnLive = document.getElementById('btn-vod-live');
            btnLive.disabled = true;
            btnLive.innerText = "Voltando para Ao Vivo...";

            try {{
                const res = await fetch('/api/vod/live', {{
                    method: 'POST',
                    headers: {{
                        'Content-Type': 'application/json',
                        'X-Auth-PIN': pin
                    }},
                    body: JSON.stringify({{ pin: pin }})
                }});
                const data = await res.json();
                if (data.success) {{
                    showToast("Retornando TV para Ao Vivo...");
                    activeVodTaskId = null;
                    pollVODStatus();
                    updateStatus();
                }} else {{
                    showToast("Erro ao retornar para Live.");
                }}
            }} catch (e) {{
                showToast("Erro de comunicação com o servidor.");
            }} finally {{
                btnLive.disabled = false;
            }}
        }}

        async function updateStatus() {{
            try {{
                const res = await fetch('/api/status');
                const data = await res.json();
                document.getElementById('active-title').innerText = data.active_channel_name;
                document.getElementById('traffic-text').innerText = `${{data.total_mb}} MB`;

                const tvPill = document.getElementById('tv-status');
                const tvText = document.getElementById('tv-status-text');
                const leadBadge = document.getElementById('client-lead-badge');
                if (data.client && data.client.last_seen_secs < 8) {{
                    const c = data.client;
                    tvPill.className = 'status-pill';
                    const leadStr = c.lead_mb > 0 ? ` +${{c.lead_mb}}MB` : '';
                    tvText.innerText = `TV Lendo (${{leadStr || 'Ao vivo'}})`;
                    if (leadBadge) {{
                        leadBadge.style.display = 'block';
                        const fuseRead = c.fuse ? (c.fuse.total_read || '0MB') : '0MB';
                        leadBadge.innerText = `Aparelho: ${{c.writer_mb}}MB | TV: ${{fuseRead}}`;
                    }}
                }} else if (data.in_standby) {{
                    tvPill.className = 'status-pill standby';
                    tvText.innerText = 'Standby (Eco)';
                    if (leadBadge) leadBadge.style.display = 'none';
                }} else if (data.listeners > 0) {{
                    tvPill.className = 'status-pill';
                    tvText.innerText = 'TV Conectada (1080p)';
                    if (leadBadge) leadBadge.style.display = 'none';
                }} else {{
                    tvPill.className = 'status-pill waiting';
                    tvText.innerText = 'Aguardando TV';
                    if (leadBadge) leadBadge.style.display = 'none';
                }}

                if (data.youtube) {{
                    const yb = document.getElementById('yt-active-box');
                    if (yb) {{
                        yb.style.display = 'block';
                        const thumb = document.getElementById('yt-thumb');
                        if (thumb) {{
                            if (data.youtube.thumbnail) {{
                                thumb.src = data.youtube.thumbnail;
                                thumb.style.display = 'block';
                            }} else {{
                                thumb.style.display = 'none';
                            }}
                        }}
                        const yTitle = document.getElementById('yt-title');
                        if (yTitle) yTitle.innerText = data.youtube.title || 'Vídeo';
                        const yMeta = document.getElementById('yt-meta-info');
                        if (yMeta) {{
                            let durStr = data.youtube.is_live ? 'AO VIVO' : (data.youtube.duration ? `${{Math.floor(data.youtube.duration / 60)}}m${{data.youtube.duration % 60}}s` : '');
                            yMeta.innerText = `YouTube • ${{durStr}} • Retorna à TV ao finalizar`;
                        }}
                    }}
                }} else {{
                    const yb = document.getElementById('yt-active-box');
                    if (yb) yb.style.display = 'none';
                }}

                if (currentActiveId !== data.active_channel_id) {{
                    currentActiveId = data.active_channel_id;
                    document.querySelectorAll('.channel-card').forEach(c => c.classList.remove('active'));
                    const curCard = document.getElementById(`card-${{currentActiveId}}`);
                    if (curCard) curCard.classList.add('active');
                }}
            }} catch (e) {{}}
        }}

        async function syncChannels() {{
            showToast("Atualizando grade com o provedor...");
            try {{
                await fetch('/api/sync', {{ method: 'POST' }});
                setTimeout(async () => {{
                    const chRes = await fetch('/api/channels');
                    const channelsObj = await chRes.json();
                    allChannels = Object.values(channelsObj);
                    applyFilters();
                    showToast("Grade de canais atualizada com sucesso!");
                }}, 4000);
            }} catch (e) {{
                showToast("Erro ao sincronizar.");
            }}
        }}

        function showToast(msg) {{
            const toast = document.getElementById('toast');
            toast.innerText = msg;
            toast.style.display = 'block';
            setTimeout(() => {{ toast.style.display = 'none'; }}, 2500);
        }}

        init();
    </script>
</body>
</html>
"""
        data = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

def run():
    server = ThreadedHTTPServer((HOST, PORT), RequestHandler)
    print("==================================================")
    print(f" [✓] Servidor Multi-Canal Ativo em http://{HOST}:{PORT}")
    print("==================================================")
    try:
        resume_interrupted_vod_tasks()
    except Exception as e:
        print(f"[!] Erro ao retomar VODs: {e}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[!] Encerrando...")
        HUB.running = False
        if HUB.proc:
            HUB.proc.kill()
        server.server_close()

if __name__ == "__main__":
    run()
